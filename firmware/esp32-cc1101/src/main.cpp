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
static const uint8_t REG_PARTNUM = 0x30;
static const uint8_t REG_VERSION = 0x31;
static const uint8_t REG_RSSI = 0x34;
static const uint8_t REG_MARCSTATE = 0x35;
static const uint8_t REG_PKTSTATUS = 0x38;
static const uint32_t CHIP_READY_TIMEOUT_US = 20000;

// Becker Centronic register set from centronic-py / FHEM SIGNALduino:
// 2-FSK at 868.283 MHz, asynchronous serial output on GDO2.
static const uint8_t BECKER_REGS[][2] = {
  {0x00, 0x0D}, {0x01, 0x2E}, {0x02, 0x2D}, {0x03, 0x47}, {0x04, 0xD3},
  {0x05, 0x91}, {0x06, 0x3D}, {0x07, 0x04}, {0x08, 0x32}, {0x09, 0x00},
  {0x0A, 0x00}, {0x0B, 0x06}, {0x0C, 0x00}, {0x0D, 0x21}, {0x0E, 0x65},
  {0x0F, 0x3F}, {0x10, 0x57}, {0x11, 0xC4}, {0x12, 0x06}, {0x13, 0x23},
  // MCSM1=0x0C keeps RX active after a detected packet instead of IDLE.
  {0x14, 0xB9}, {0x15, 0x40}, {0x16, 0x07}, {0x17, 0x0C}, {0x18, 0x18},
  {0x19, 0x14}, {0x1A, 0x6C}, {0x1B, 0x00}, {0x1C, 0x00}, {0x1D, 0x92},
  {0x1E, 0x87}, {0x1F, 0x6B}, {0x20, 0xF8}, {0x21, 0xB6}, {0x22, 0x11},
  {0x23, 0xEF}, {0x24, 0x2B}, {0x25, 0x14}, {0x26, 0x1F}, {0x27, 0x41},
  {0x28, 0x00}, {0x29, 0x59}, {0x2A, 0x7F}, {0x2B, 0x07}, {0x2C, 0x88},
  {0x2D, 0x31}, {0x2E, 0x0B},
};

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

  Serial.println("CC1101 ready: 868.283 MHz 2-FSK, async data on GDO2/GPIO4");
  return true;
}

// ---------- asynchronous pulse capture ----------
static const size_t MAX_PULSES = 512;
static const uint32_t FRAME_GAP_US = 2500;
static const uint32_t MIN_GLITCH_US = 80;

static volatile uint16_t pulseDurations[MAX_PULSES];
static volatile uint8_t pulseLevels[MAX_PULSES];
static volatile size_t pulseCount = 0;
static volatile uint32_t lastEdgeUs = 0;
static volatile bool pulseOverflow = false;
static volatile uint32_t totalEdgeCount = 0;
static portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR onGdo2Edge() {
  const uint32_t now = micros();
  const uint32_t duration = now - lastEdgeUs;
  const uint8_t previousLevel = digitalRead(PIN_GDO2) == LOW ? HIGH : LOW;
  lastEdgeUs = now;

  portENTER_CRITICAL_ISR(&pulseMux);
  totalEdgeCount++;
  if (duration > FRAME_GAP_US) {
    // The loop normally consumes the preceding frame during the idle gap.
    // If it did not, start clean rather than merging two transmissions.
    pulseCount = 0;
    pulseOverflow = false;
  } else if (duration >= MIN_GLITCH_US) {
    if (pulseCount < MAX_PULSES) {
      pulseDurations[pulseCount] = static_cast<uint16_t>(duration);
      pulseLevels[pulseCount] = previousLevel;
      pulseCount++;
    } else {
      pulseOverflow = true;
    }
  }
  portEXIT_CRITICAL_ISR(&pulseMux);
}

static bool takePulseFrame(uint16_t* durations, uint8_t* levels,
                           size_t& count, bool& overflow) {
  const uint32_t now = micros();
  if (pulseCount == 0 || now - lastEdgeUs < FRAME_GAP_US) return false;

  portENTER_CRITICAL(&pulseMux);
  if (pulseCount == 0 || now - lastEdgeUs < FRAME_GAP_US) {
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

static void processPulseFrame(const uint16_t* durations, const uint8_t* levels,
                              size_t count, bool overflow) {
  if (count < 2) return;

  uint32_t totalDuration = 0;
  size_t timingMatches = 0;
  uint32_t clockSum = 0;
  size_t clockSamples = 0;
  for (size_t i = 0; i < count; i++) {
    const uint16_t duration = durations[i];
    totalDuration += duration;
    if (duration >= 250 && duration <= 600) {
      timingMatches++;
      clockSum += duration;
      clockSamples++;
    } else if (duration >= 650 && duration <= 1100) {
      timingMatches++;
      clockSum += duration / 2;
      clockSamples++;
    }
  }

  const uint32_t estimatedClock = clockSamples == 0 ? 0 : clockSum / clockSamples;
  uint32_t estimatedHalfBits = 0;
  if (estimatedClock != 0) {
    for (size_t i = 0; i < count; i++) {
      const uint32_t units = (durations[i] + estimatedClock / 2) / estimatedClock;
      if (units >= 1 && units <= 4) estimatedHalfBits += units;
    }
  }

  const uint32_t matchPercent = count == 0 ? 0 : timingMatches * 100 / count;
  String frame = overflow ? "BURST_OVERFLOW" : "BURST";
  frame += " n=" + String(count);
  frame += " total_us=" + String(totalDuration);
  frame += " match=" + String(matchPercent) + "%";
  frame += " c=" + String(estimatedClock);
  frame += " halfbits=" + String(estimatedHalfBits);
  frame += " bits_est=" + String(estimatedHalfBits / 2.0f, 1);
  frame += " D=";
  frame.reserve(frame.length() + count * 6);
  for (size_t i = 0; i < count; i++) {
    if (i != 0) frame += ',';
    if (levels[i] == LOW) frame += '-';
    frame += String(durations[i]);
  }
  storeFrame(frame);
}

static void maintainReceiveMode() {
  static uint32_t lastCheckMs = 0;
  const uint32_t now = millis();
  if (now - lastCheckMs < 500) return;
  lastCheckMs = now;

  uint8_t marcState = 0;
  if (!readStatusReg(REG_MARCSTATE, marcState)) return;
  if ((marcState & 0x1F) == 0x01) { // IDLE
    strobe(STROBE_SRX);
  }
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

  uint8_t rawRssi = 0;
  uint8_t marcState = 0;
  uint8_t packetStatus = 0;
  if (!readStatusReg(REG_RSSI, rawRssi) ||
      !readStatusReg(REG_MARCSTATE, marcState) ||
      !readStatusReg(REG_PKTSTATUS, packetStatus)) {
    Serial.println("DIAG SPI read failed");
    return;
  }

  const int rssiDbm = static_cast<int8_t>(rawRssi) / 2 - 74;
  Serial.printf("DIAG edges=%lu delta=%lu/2s gdo2=%d marc=0x%02X rssi=%ddBm pkt=0x%02X\n",
                static_cast<unsigned long>(edges),
                static_cast<unsigned long>(edges - previousEdgeCount),
                digitalRead(PIN_GDO2), marcState & 0x1F, rssiDbm, packetStatus);
  previousEdgeCount = edges;
}

static void pollPulseCapture() {
  static uint16_t durations[MAX_PULSES];
  static uint8_t levels[MAX_PULSES];
  size_t count = 0;
  bool overflow = false;
  if (takePulseFrame(durations, levels, count, overflow)) {
    processPulseFrame(durations, levels, count, overflow);
  }
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
    status += "GET /frames - signed GDO2 pulse durations\n";
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
  Serial.println("Ready. DIAG reports every 2s; press a Becker remote button.");
}

void loop() {
  if (wifiConnected) server.handleClient();
  if (cc1101Present) {
    maintainReceiveMode();
    pollPulseCapture();
    printRadioDiagnostic();
  }
  delay(1);
}
