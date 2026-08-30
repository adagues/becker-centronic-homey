/**
 * Becker Centronic raw capture with ESP32 + CC1101.
 *
 * The Becker/SIGNALduino register set uses asynchronous serial mode
 * (PKTCTRL0=0x32). Demodulated data is therefore exposed on GDO2; it is not
 * available through the CC1101 RX FIFO. This firmware records GDO2 edge
 * timings and emits SIGNALduino-style signed pulse durations over serial and
 * GET /frames.
 *
 * Wiring for Alexis' 2.0 mm-pitch module, components facing up and antenna at
 * the top (module pads 1..8 from left to right):
 *   1 VCC   -> ESP32 3V3 (NEVER 5V)
 *   2 GND   -> ESP32 GND
 *   3 MOSI  -> ESP32 GPIO23
 *   4 SCLK  -> ESP32 GPIO18
 *   5 MISO  -> ESP32 GPIO19
 *   6 GDO2  -> ESP32 GPIO4  (asynchronous receive data)
 *   7 GDO0  -> not connected
 *   8 CSN   -> ESP32 GPIO5
 */

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>

#include "manchester_detector.h"

// ---------- user configuration ----------
static const char* WIFI_SSID = "CHANGE_ME";
static const char* WIFI_PASS = "CHANGE_ME";

// ---------- wiring ----------
static const int PIN_SCK = 18;
static const int PIN_MISO = 19;
static const int PIN_MOSI = 23;
static const int PIN_CS = 5;
static const int PIN_GDO2 = 4;

// ---------- CC1101 low-level ----------
static const uint8_t STROBE_SRES = 0x30;
static const uint8_t STROBE_SRX = 0x34;
static const uint8_t STROBE_SIDLE = 0x36;
static const uint8_t STROBE_SFRX = 0x3A;
static const uint8_t REG_PARTNUM = 0x30;
static const uint8_t REG_VERSION = 0x31;
static const uint8_t REG_RSSI = 0x34;
static const uint8_t REG_MARCSTATE = 0x35;
static const uint8_t REG_PKTSTATUS = 0x38;
static const uint32_t CHIP_READY_TIMEOUT_US = 20000;

// Becker Centronic register set from centronic-py / FHEM SIGNALduino:
// 2-FSK at 868.283 MHz, asynchronous serial output on GDO2.
// Measured on Alexis' remote (scanner idle-vs-press sweep, 2026-08-30):
// the carrier is NOT at the published 868.282806 MHz but about 40-65 kHz above,
// peaking at 868.300/868.350 MHz with a 64 dB rise while transmitting. With the
// centre set to 868.283 both FSK tones landed on the same side of the
// discriminator, so GDO2 stayed static and nothing could be decoded.
// The probe pass then showed 2-FSK produces thousands of Becker-range pulses at
// 868.350 MHz while OOK produces none, which fixes both frequency and modulation.
static const uint8_t BECKER_REGS[][2] = {
  {0x00, 0x0D}, {0x01, 0x2E}, {0x02, 0x2D}, {0x03, 0x47}, {0x04, 0xD3},
  {0x05, 0x91}, {0x06, 0x3D}, {0x07, 0x04}, {0x08, 0x32}, {0x09, 0x00},
  // FREQ2/1/0 = 0x2165E8 -> 868.349854 MHz
  {0x0A, 0x00}, {0x0B, 0x06}, {0x0C, 0x00}, {0x0D, 0x21}, {0x0E, 0x65},
  {0x0F, 0xE8},
  // MDMCFG4=0x86 -> 203 kHz RX filter, MDMCFG3=0x83 -> 2399 baud,
  // MDMCFG2=0x00 -> 2-FSK with no sync-word gating, DEVIATN=0x40 -> 25.4 kHz.
  {0x10, 0x86}, {0x11, 0x83}, {0x12, 0x00}, {0x13, 0x23},
  // MCSM1=0x00 as in the proven SIGNALduino profile.
  {0x14, 0xB9}, {0x15, 0x40}, {0x16, 0x07}, {0x17, 0x00}, {0x18, 0x18},
  {0x19, 0x14}, {0x1A, 0x6C}, {0x1B, 0x07}, {0x1C, 0x00}, {0x1D, 0x91},
  {0x1E, 0x87}, {0x1F, 0x6B}, {0x20, 0xF8}, {0x21, 0xB6}, {0x22, 0x11},
  {0x23, 0xE9}, {0x24, 0x2A}, {0x25, 0x00}, {0x26, 0x1F}, {0x27, 0x41},
  {0x28, 0x00}, {0x29, 0x59}, {0x2A, 0x7F}, {0x2B, 0x07}, {0x2C, 0x88},
  {0x2D, 0x31}, {0x2E, 0x0B},
};

// Runtime retune, so the exact centre can be trimmed without reflashing.
static const double CRYSTAL_HZ = 26000000.0;
static double centreFrequencyHz = 868349854.0;

static bool cc1101Present = false;
static uint8_t cc1101Partnum = 0xFF;
static uint8_t cc1101Version = 0xFF;

static bool waitForChipReady() {
  const uint32_t started = micros();
  while (digitalRead(PIN_MISO) != LOW) {
    if (micros() - started >= CHIP_READY_TIMEOUT_US) return false;
  }
  return true;
}

static bool selectChip() {
  digitalWrite(PIN_CS, LOW);
  if (waitForChipReady()) return true;
  digitalWrite(PIN_CS, HIGH);
  return false;
}

static bool spiTransfer(uint8_t address, uint8_t value, uint8_t& result) {
  if (!selectChip()) return false;
  SPI.transfer(address);
  result = SPI.transfer(value);
  digitalWrite(PIN_CS, HIGH);
  return true;
}

static bool strobe(uint8_t command) {
  if (!selectChip()) return false;
  SPI.transfer(command);
  digitalWrite(PIN_CS, HIGH);
  return true;
}

static bool writeReg(uint8_t address, uint8_t value) {
  uint8_t ignored = 0;
  return spiTransfer(address, value, ignored);
}

static bool readStatusReg(uint8_t address, uint8_t& value) {
  return spiTransfer(address | 0xC0, 0x00, value);
}

static bool cc1101Init() {
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_MISO, INPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  // Datasheet reset sequence: CS toggle, wait for SO/MISO low, then SRES.
  delay(1);
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(10);
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(50);
  if (!strobe(STROBE_SRES)) {
    Serial.println("CC1101 ERROR: no SPI response (MISO stayed high)");
    return false;
  }
  delay(1);

  if (!readStatusReg(REG_PARTNUM, cc1101Partnum) ||
      !readStatusReg(REG_VERSION, cc1101Version)) {
    Serial.println("CC1101 ERROR: unable to read identity registers");
    return false;
  }

  Serial.printf("CC1101 identity: PARTNUM=0x%02X VERSION=0x%02X\n",
                cc1101Partnum, cc1101Version);
  if (cc1101Partnum != 0x00 || cc1101Version == 0x00 ||
      cc1101Version == 0xFF) {
    Serial.println("CC1101 ERROR: invalid identity; check VCC/GND/MISO/CSN");
    return false;
  }

  for (const auto& reg : BECKER_REGS) {
    if (!writeReg(reg[0], reg[1])) {
      Serial.printf("CC1101 ERROR: SPI timeout writing register 0x%02X\n", reg[0]);
      return false;
    }
  }
  if (!strobe(STROBE_SRX)) {
    Serial.println("CC1101 ERROR: unable to enter receive mode");
    return false;
  }

  Serial.printf("CC1101 ready: %.6f MHz 2-FSK, async data on GDO2/GPIO4\n",
                centreFrequencyHz / 1e6);
  return true;
}

// ---------- asynchronous pulse capture ----------
static const size_t MAX_PULSES = 512;
static const size_t STREAM_CHUNK_PULSES = 64;
static const uint32_t FRAME_GAP_US = 2500;
static const uint32_t MIN_CAPTURE_PULSE_US = 20;

static volatile uint16_t pulseDurations[MAX_PULSES];
static volatile uint8_t pulseLevels[MAX_PULSES];
static volatile size_t pulseCount = 0;
static volatile uint32_t lastEdgeUs = 0;
static volatile bool pulseOverflow = false;
static volatile uint32_t totalEdgeCount = 0;
static portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

static becker::ManchesterDetector manchesterDetector;
static uint32_t manchesterDetected = 0;
static uint32_t manchesterRejected = 0;
static uint32_t captureOverflows = 0;

static void IRAM_ATTR onGdo2Edge() {
  const uint32_t now = micros();
  const uint32_t duration = now - lastEdgeUs;
  const uint8_t previousLevel = digitalRead(PIN_GDO2) == LOW ? HIGH : LOW;
  lastEdgeUs = now;

  portENTER_CRITICAL_ISR(&pulseMux);
  totalEdgeCount++;
  if (duration >= MIN_CAPTURE_PULSE_US) {
    if (pulseCount < MAX_PULSES) {
      pulseDurations[pulseCount] =
          static_cast<uint16_t>(duration > UINT16_MAX ? UINT16_MAX : duration);
      pulseLevels[pulseCount] = previousLevel;
      pulseCount++;
    } else {
      pulseOverflow = true;
    }
  }
  portEXIT_CRITICAL_ISR(&pulseMux);
}

static bool takePulseChunk(uint16_t* durations, uint8_t* levels,
                           size_t& count, bool& overflow) {
  const uint32_t now = micros();
  const size_t observedCount = pulseCount;
  const bool idleGap = observedCount > 0 && now - lastEdgeUs >= FRAME_GAP_US;
  if (observedCount < STREAM_CHUNK_PULSES && !idleGap && !pulseOverflow) {
    return false;
  }

  portENTER_CRITICAL(&pulseMux);
  const bool lockedIdleGap = pulseCount > 0 && now - lastEdgeUs >= FRAME_GAP_US;
  if (pulseCount < STREAM_CHUNK_PULSES && !lockedIdleGap && !pulseOverflow) {
    portEXIT_CRITICAL(&pulseMux);
    return false;
  }

  count = pulseCount;
  overflow = pulseOverflow;
  for (size_t i = 0; i < count; i++) {
    durations[i] = pulseDurations[i];
    levels[i] = pulseLevels[i];
  }
  pulseCount = 0;
  pulseOverflow = false;
  portEXIT_CRITICAL(&pulseMux);
  return true;
}

// ---------- capture buffer ----------
static const int MAX_FRAMES = 16;
static String frames[MAX_FRAMES];
static int frameCount = 0;

static void storeFrame(const String& frame) {
  if (frameCount < MAX_FRAMES) {
    frames[frameCount++] = frame;
  } else {
    for (int i = 1; i < MAX_FRAMES; i++) frames[i - 1] = frames[i];
    frames[MAX_FRAMES - 1] = frame;
  }
  Serial.println(frame);
}

static String bitsToHex(const becker::ManchesterResult& result, bool invert) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  String output;
  output.reserve((result.bitCount + 3) / 4);
  for (size_t firstBit = 0; firstBit < result.bitCount; firstBit += 4) {
    uint8_t nibble = 0;
    for (size_t offset = 0; offset < 4; offset++) {
      nibble <<= 1;
      const size_t bitIndex = firstBit + offset;
      if (bitIndex < result.bitCount) {
        nibble |= result.bits[bitIndex] ^ (invert ? 1 : 0);
      }
    }
    output += HEX_DIGITS[nibble];
  }
  return output;
}

static void handleManchesterEvent(becker::ManchesterEvent event,
                                  const becker::ManchesterResult& result) {
  if (event == becker::ManchesterEvent::None) return;
  if (event == becker::ManchesterEvent::Rejected) {
    manchesterRejected++;
    return;
  }

  manchesterDetected++;
  String frame = "MC bits=" + String(result.bitCount);
  frame += " c=" + String(result.clockUs);
  frame += " pulses=" + String(result.pulseCount);
  frame += " halfbits=" + String(result.halfBitCount);
  frame += " glitches=" + String(result.glitchCount);
  frame += " phase=" + String(result.phase);
  frame += " hex=" + bitsToHex(result, false);
  frame += " inv=" + bitsToHex(result, true);
  frame += " D=";
  frame.reserve(frame.length() + result.pulseCount * 6);
  for (size_t i = 0; i < result.pulseCount; i++) {
    if (i != 0) frame += ',';
    if (result.levels[i] == LOW) frame += '-';
    frame += String(result.durations[i]);
  }
  storeFrame(frame);
}

static void pollPulseCapture() {
  static uint16_t durations[MAX_PULSES];
  static uint8_t levels[MAX_PULSES];
  becker::ManchesterResult result;
  size_t count = 0;
  bool overflow = false;

  while (takePulseChunk(durations, levels, count, overflow)) {
    if (overflow) {
      captureOverflows++;
      manchesterDetector.reset();
    }
    for (size_t i = 0; i < count; i++) {
      const becker::ManchesterEvent event =
          manchesterDetector.push(durations[i], levels[i], result);
      handleManchesterEvent(event, result);
    }
    count = 0;
    overflow = false;
  }

  if (micros() - lastEdgeUs >= FRAME_GAP_US) {
    handleManchesterEvent(manchesterDetector.flush(result), result);
  }
}

static bool waitForMarcState(uint8_t expectedState, uint32_t timeoutUs = 20000) {
  const uint32_t started = micros();
  do {
    uint8_t marcState = 0;
    if (readStatusReg(REG_MARCSTATE, marcState) &&
        (marcState & 0x1F) == expectedState) {
      return true;
    }
    delayMicroseconds(50);
  } while (micros() - started < timeoutUs);
  return false;
}

static bool recoverReceiveMode() {
  // SFRX is only legal in IDLE. Wait for each MARC transition instead of
  // issuing three strobes back-to-back while the radio is still in RX_RST.
  if (!strobe(STROBE_SIDLE) || !waitForMarcState(0x01)) return false;
  if (!strobe(STROBE_SFRX)) return false;
  if (!strobe(STROBE_SRX) || !waitForMarcState(0x0D)) return false;

  portENTER_CRITICAL(&pulseMux);
  pulseCount = 0;
  pulseOverflow = false;
  lastEdgeUs = micros();
  portEXIT_CRITICAL(&pulseMux);
  manchesterDetector.reset();
  return true;
}

static bool applyCentreFrequency() {
  const uint32_t word =
      static_cast<uint32_t>((centreFrequencyHz / CRYSTAL_HZ) * 65536.0 + 0.5);
  if (!strobe(STROBE_SIDLE) || !waitForMarcState(0x01)) return false;
  if (!writeReg(0x0D, (word >> 16) & 0xFF)) return false;
  if (!writeReg(0x0E, (word >> 8) & 0xFF)) return false;
  if (!writeReg(0x0F, word & 0xFF)) return false;
  if (!strobe(STROBE_SRX) || !waitForMarcState(0x0D)) return false;

  portENTER_CRITICAL(&pulseMux);
  pulseCount = 0;
  pulseOverflow = false;
  lastEdgeUs = micros();
  portEXIT_CRITICAL(&pulseMux);
  manchesterDetector.reset();
  Serial.printf("FREQ set to %.6f MHz\n", centreFrequencyHz / 1e6);
  return true;
}

static void maintainReceiveMode() {
  // The RF review asked for no SPI traffic during capture beyond a rare state
  // check: in asynchronous serial mode the RX FIFO is unused, so draining it
  // only added noise and state-machine churn.
  static uint32_t lastStateCheckMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastStateCheckMs < 2000) return;
  lastStateCheckMs = nowMs;

  uint8_t marcState = 0;
  if (!readStatusReg(REG_MARCSTATE, marcState)) return;
  const uint8_t state = marcState & 0x1F;
  if (state == 0x01) {
    strobe(STROBE_SRX);
  } else if (state != 0x0D) {
    recoverReceiveMode();
  }
}

static void handleSerialCommand() {
  while (Serial.available() > 0) {
    switch (static_cast<char>(Serial.read())) {
      case '+':
        centreFrequencyHz += 10000.0;
        applyCentreFrequency();
        break;
      case '-':
        centreFrequencyHz -= 10000.0;
        applyCentreFrequency();
        break;
      case 'i':
        Serial.printf("INFO f=%.6f MHz mc=%lu reject=%lu drop=%lu glitches=%lu\n",
                      centreFrequencyHz / 1e6,
                      static_cast<unsigned long>(manchesterDetected),
                      static_cast<unsigned long>(manchesterRejected),
                      static_cast<unsigned long>(captureOverflows),
                      static_cast<unsigned long>(
                          manchesterDetector.mergedGlitchCount()));
        break;
      case 'r':
        recoverReceiveMode();
        Serial.println(F("receive path re-armed"));
        break;
      default:
        break;
    }
  }
}

// A remote burst lasts only tens of milliseconds, so reading RSSI once per
// report almost always misses it. Track the peak continuously instead, and skip
// sampling while a pulse train is active so SPI traffic cannot disturb it.
static int rssiPeakDbm = -127;

static void trackRssiPeak() {
  static uint32_t lastSampleMs = 0;
  const uint32_t now = millis();
  if (now - lastSampleMs < 10) return;
  if (micros() - lastEdgeUs < 5000) return;
  lastSampleMs = now;

  uint8_t rawRssi = 0;
  if (!readStatusReg(REG_RSSI, rawRssi)) return;
  const int dbm = static_cast<int8_t>(rawRssi) / 2 - 74;
  if (dbm > rssiPeakDbm) rssiPeakDbm = dbm;
}

static void printRadioDiagnostic() {
  static uint32_t lastReportMs = 0;
  static uint32_t previousEdgeCount = 0;
  const uint32_t now = millis();
  if (now - lastReportMs < 2000) return;
  lastReportMs = now;

  uint32_t edges = 0;
  portENTER_CRITICAL(&pulseMux);
  edges = totalEdgeCount;
  portEXIT_CRITICAL(&pulseMux);

  uint8_t marcState = 0;
  uint8_t packetStatus = 0;
  if (!readStatusReg(REG_MARCSTATE, marcState) ||
      !readStatusReg(REG_PKTSTATUS, packetStatus)) {
    Serial.println("DIAG SPI read failed");
    return;
  }

  const int rssiDbm = rssiPeakDbm;
  rssiPeakDbm = -127;
  Serial.printf("DIAG edges=%lu delta=%lu/2s gdo2=%d marc=0x%02X peak_rssi=%ddBm pkt=0x%02X mc=%lu reject=%lu drop=%lu glitches=%lu\n",
                static_cast<unsigned long>(edges),
                static_cast<unsigned long>(edges - previousEdgeCount),
                digitalRead(PIN_GDO2), marcState & 0x1F, rssiDbm, packetStatus,
                static_cast<unsigned long>(manchesterDetected),
                static_cast<unsigned long>(manchesterRejected),
                static_cast<unsigned long>(captureOverflows),
                static_cast<unsigned long>(manchesterDetector.mergedGlitchCount()));
  previousEdgeCount = edges;
}

// ---------- optional Wi-Fi / HTTP ----------
static WebServer server(80);
static bool wifiConnected = false;

static void startWifi() {
  if (strcmp(WIFI_SSID, "CHANGE_ME") == 0 || WIFI_SSID[0] == '\0') {
    Serial.println("WiFi disabled: set WIFI_SSID/WIFI_PASS for GET /frames");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi unavailable; serial capture remains active");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  wifiConnected = true;
  Serial.println("\nIP: " + WiFi.localIP().toString());
  server.on("/", HTTP_GET, []() {
    String status = "Becker CC1101 raw capture\n";
    status += "CC1101 PARTNUM=0x" + String(cc1101Partnum, HEX);
    status += " VERSION=0x" + String(cc1101Version, HEX) + "\n";
    status += "GET /frames - detected Manchester frames with raw timings\n";
    status += "POST /clear - clear stored captures\n";
    server.send(200, "text/plain", status);
  });
  server.on("/frames", HTTP_GET, []() {
    String output;
    for (int i = 0; i < frameCount; i++) output += frames[i] + "\n";
    server.send(200, "text/plain", output);
  });
  server.on("/clear", HTTP_POST, []() {
    frameCount = 0;
    server.send(200, "text/plain", "cleared\n");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nBecker CC1101 raw capture starting");

  cc1101Present = cc1101Init();
  if (cc1101Present) {
    pinMode(PIN_GDO2, INPUT);
    lastEdgeUs = micros();
    attachInterrupt(digitalPinToInterrupt(PIN_GDO2), onGdo2Edge, CHANGE);
  } else {
    Serial.println("Capture disabled until a valid CC1101 is detected");
  }

  startWifi();
  Serial.printf("Ready at %.6f MHz. Commands: +/- retune 10 kHz, i=info, r=rearm\n",
                centreFrequencyHz / 1e6);
  Serial.println("DIAG reports every 2s; press a Becker remote button.");
}

void loop() {
  handleSerialCommand();
  if (wifiConnected) server.handleClient();
  if (cc1101Present) {
    maintainReceiveMode();
    pollPulseCapture();
    trackRssiPeak();
    printRadioDiagnostic();
  }
  delay(1);
}
