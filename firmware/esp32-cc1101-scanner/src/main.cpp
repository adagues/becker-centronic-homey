/**
 * Becker Centronic RF scanner for ESP32 + CC1101 (868 MHz).
 *
 * Why this exists: with the published Becker profile (868.282806 MHz, 2-FSK)
 * the remote is received very strongly (RSSI about -37 dBm) but the demodulator
 * produces no data transitions on GDO2. That pattern fits a carrier that is
 * inside the wide 325 kHz receive filter yet off the tuned centre, or a
 * modulation that is not 2-FSK.
 *
 * Two modes, switchable over serial:
 *   SCAN  - narrow filter, sweep 867.70 .. 869.10 MHz and report where the
 *           remote's carrier actually peaks.
 *   PROBE - stay on the strongest frequency and cycle modulation / data-rate /
 *           deviation candidates, counting Becker-like pulse widths on GDO2.
 *
 * Wiring (unchanged):
 *   CC1101 VCC -> 3V3 (never 5V)   GND  -> GND
 *   MOSI -> GPIO23   SCLK -> GPIO18   MISO -> GPIO19
 *   GDO2 -> GPIO4    GDO0 -> not connected   CSN -> GPIO5
 *
 * Serial: 115200 baud. Commands (one character + Enter):
 *   s  scan mode        p  probe mode
 *   t  print table      z  zero statistics
 *
 * No Wi-Fi: it caused brownout resets on this board.
 */

#include <Arduino.h>
#include <SPI.h>

// ---------- wiring ----------
static const int PIN_SCK = 18;
static const int PIN_MISO = 19;
static const int PIN_MOSI = 23;
static const int PIN_CS = 5;
static const int PIN_GDO2 = 4;

// ---------- CC1101 ----------
static const uint8_t STROBE_SRES = 0x30;
static const uint8_t STROBE_SRX = 0x34;
static const uint8_t STROBE_SIDLE = 0x36;
static const uint8_t REG_PARTNUM = 0x30;
static const uint8_t REG_VERSION = 0x31;
static const uint8_t REG_FREQEST = 0x32;
static const uint8_t REG_RSSI = 0x34;
static const uint8_t REG_MARCSTATE = 0x35;
static const uint32_t CHIP_READY_TIMEOUT_US = 20000;
static const double CRYSTAL_HZ = 26000000.0;

// Becker/SIGNALduino base profile. FREQ2/1/0, MDMCFG4/3/2 and DEVIATN are
// overwritten while scanning and probing.
static const uint8_t BASE_REGS[][2] = {
  {0x00, 0x0D}, {0x01, 0x2E}, {0x02, 0x2D}, {0x03, 0x47}, {0x04, 0xD3},
  {0x05, 0x91}, {0x06, 0x3D}, {0x07, 0x04}, {0x08, 0x32}, {0x09, 0x00},
  {0x0A, 0x00}, {0x0B, 0x06}, {0x0C, 0x00}, {0x0D, 0x21}, {0x0E, 0x65},
  {0x0F, 0x3F}, {0x10, 0x57}, {0x11, 0xC4}, {0x12, 0x06}, {0x13, 0x23},
  {0x14, 0xB9}, {0x15, 0x40}, {0x16, 0x07}, {0x17, 0x00}, {0x18, 0x18},
  {0x19, 0x14}, {0x1A, 0x6C}, {0x1B, 0x07}, {0x1C, 0x00}, {0x1D, 0x91},
  {0x1E, 0x87}, {0x1F, 0x6B}, {0x20, 0xF8}, {0x21, 0xB6}, {0x22, 0x11},
  {0x23, 0xE9}, {0x24, 0x2A}, {0x25, 0x00}, {0x26, 0x1F}, {0x27, 0x41},
  {0x28, 0x00}, {0x2C, 0x88}, {0x2D, 0x31}, {0x2E, 0x0B},
};

static uint8_t cc1101Partnum = 0xFF;
static uint8_t cc1101Version = 0xFF;

static bool waitChipReady() {
  const uint32_t started = micros();
  while (digitalRead(PIN_MISO) != LOW) {
    if (micros() - started >= CHIP_READY_TIMEOUT_US) return false;
  }
  return true;
}

static bool select() {
  digitalWrite(PIN_CS, LOW);
  if (waitChipReady()) return true;
  digitalWrite(PIN_CS, HIGH);
  return false;
}

static bool transfer(uint8_t address, uint8_t value, uint8_t& result) {
  if (!select()) return false;
  SPI.transfer(address);
  result = SPI.transfer(value);
  digitalWrite(PIN_CS, HIGH);
  return true;
}

static bool strobe(uint8_t command) {
  if (!select()) return false;
  SPI.transfer(command);
  digitalWrite(PIN_CS, HIGH);
  return true;
}

static bool writeReg(uint8_t address, uint8_t value) {
  uint8_t ignored = 0;
  return transfer(address, value, ignored);
}

static bool readStatus(uint8_t address, uint8_t& value) {
  return transfer(address | 0xC0, 0x00, value);
}

static int rssiDbm(uint8_t raw) { return static_cast<int8_t>(raw) / 2 - 74; }

// ---------- GDO2 pulse statistics ----------
// Becker reference timings: half-bit about 414 us, full bit about 828 us.
static const uint32_t BECKER_MIN_US = 250;
static const uint32_t BECKER_MAX_US = 1150;

static volatile uint32_t edgeCount = 0;
static volatile uint32_t beckerPulseCount = 0;
static volatile uint32_t lastEdgeUs = 0;
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR onGdo2Edge() {
  const uint32_t now = micros();
  const uint32_t width = now - lastEdgeUs;
  lastEdgeUs = now;
  portENTER_CRITICAL_ISR(&statsMux);
  edgeCount++;
  if (width >= BECKER_MIN_US && width <= BECKER_MAX_US) beckerPulseCount++;
  portEXIT_CRITICAL_ISR(&statsMux);
}

static void readAndClearStats(uint32_t& edges, uint32_t& beckerPulses) {
  portENTER_CRITICAL(&statsMux);
  edges = edgeCount;
  beckerPulses = beckerPulseCount;
  edgeCount = 0;
  beckerPulseCount = 0;
  portEXIT_CRITICAL(&statsMux);
}

// ---------- radio helpers ----------
static bool applyBaseProfile() {
  for (const auto& reg : BASE_REGS) {
    if (!writeReg(reg[0], reg[1])) return false;
  }
  return true;
}

static bool tune(double frequencyHz) {
  const uint32_t word =
      static_cast<uint32_t>((frequencyHz / CRYSTAL_HZ) * 65536.0 + 0.5);
  if (!strobe(STROBE_SIDLE)) return false;
  delayMicroseconds(200);
  if (!writeReg(0x0D, (word >> 16) & 0xFF)) return false;
  if (!writeReg(0x0E, (word >> 8) & 0xFF)) return false;
  if (!writeReg(0x0F, word & 0xFF)) return false;
  // MCSM0 keeps FS_AUTOCAL on IDLE->RX, so SRX recalibrates for the new channel.
  if (!strobe(STROBE_SRX)) return false;
  return true;
}

static bool setModem(uint8_t mdmcfg4, uint8_t mdmcfg3, uint8_t mdmcfg2,
                     uint8_t deviatn) {
  if (!strobe(STROBE_SIDLE)) return false;
  delayMicroseconds(200);
  if (!writeReg(0x10, mdmcfg4)) return false;
  if (!writeReg(0x11, mdmcfg3)) return false;
  if (!writeReg(0x12, mdmcfg2)) return false;
  if (!writeReg(0x15, deviatn)) return false;
  if (!strobe(STROBE_SRX)) return false;
  return true;
}

static bool cc1101Init() {
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_MISO, INPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  delay(1);
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(10);
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(50);
  if (!strobe(STROBE_SRES)) {
    Serial.println(F("CC1101 ERROR: no SPI response"));
    return false;
  }
  delay(1);

  if (!readStatus(REG_PARTNUM, cc1101Partnum) ||
      !readStatus(REG_VERSION, cc1101Version)) {
    Serial.println(F("CC1101 ERROR: identity read failed"));
    return false;
  }
  Serial.printf("CC1101 identity: PARTNUM=0x%02X VERSION=0x%02X\n",
                cc1101Partnum, cc1101Version);
  if (cc1101Partnum != 0x00 || cc1101Version == 0x00 ||
      cc1101Version == 0xFF) {
    Serial.println(F("CC1101 ERROR: invalid identity; check wiring"));
    return false;
  }
  if (!applyBaseProfile()) {
    Serial.println(F("CC1101 ERROR: register write failed"));
    return false;
  }
  return strobe(STROBE_SRX);
}

// ---------- scan mode ----------
static const double SCAN_START_HZ = 867700000.0;
static const double SCAN_STEP_HZ = 50000.0;
static const size_t SCAN_BINS = 29;   // 867.70 .. 869.10 MHz
static const uint32_t SCAN_SETTLE_MS = 3;
static const uint32_t SCAN_DWELL_MS = 22;
// Narrow filter (101.5 kHz) so a strong carrier only shows up near its channel.
static const uint8_t SCAN_MDMCFG4 = 0xC6;
static const uint8_t SCAN_MDMCFG3 = 0x83;   // about 2400 baud
static const uint8_t SCAN_MDMCFG2 = 0x00;   // 2-FSK, no sync gating
static const uint8_t SCAN_DEVIATN = 0x40;   // about 25 kHz
static const int HIT_THRESHOLD_DBM = -65;

static int binMaxRssi[SCAN_BINS];
static uint32_t binBeckerPulses[SCAN_BINS];
static uint32_t binEdges[SCAN_BINS];
static size_t scanIndex = 0;
static uint32_t sweepCount = 0;

static double binFrequency(size_t index) {
  return SCAN_START_HZ + static_cast<double>(index) * SCAN_STEP_HZ;
}

static void resetStats() {
  for (size_t i = 0; i < SCAN_BINS; i++) {
    binMaxRssi[i] = -127;
    binBeckerPulses[i] = 0;
    binEdges[i] = 0;
  }
  sweepCount = 0;
  uint32_t edges = 0;
  uint32_t pulses = 0;
  readAndClearStats(edges, pulses);
  Serial.println(F("statistics cleared"));
}

static void printTable() {
  Serial.printf("TABLE sweeps=%lu\n", static_cast<unsigned long>(sweepCount));
  size_t best = 0;
  for (size_t i = 0; i < SCAN_BINS; i++) {
    if (binMaxRssi[i] > binMaxRssi[best]) best = i;
  }
  for (size_t i = 0; i < SCAN_BINS; i++) {
    if (binMaxRssi[i] == -127 && binEdges[i] == 0) continue;
    Serial.printf("BIN f=%.3f MHz max_rssi=%d dBm becker=%lu edges=%lu%s\n",
                  binFrequency(i) / 1e6, binMaxRssi[i],
                  static_cast<unsigned long>(binBeckerPulses[i]),
                  static_cast<unsigned long>(binEdges[i]),
                  i == best ? "  <== strongest" : "");
  }
  Serial.printf("BEST f=%.3f MHz max_rssi=%d dBm\n", binFrequency(best) / 1e6,
                binMaxRssi[best]);
}

static void scanStep() {
  const size_t index = scanIndex;
  if (!tune(binFrequency(index))) {
    Serial.println(F("SCAN ERROR: tune failed"));
    return;
  }
  delay(SCAN_SETTLE_MS);

  uint32_t edges = 0;
  uint32_t pulses = 0;
  readAndClearStats(edges, pulses);

  int peak = -127;
  const uint32_t until = millis() + SCAN_DWELL_MS;
  while (static_cast<int32_t>(until - millis()) > 0) {
    uint8_t raw = 0;
    if (readStatus(REG_RSSI, raw)) {
      const int dbm = rssiDbm(raw);
      if (dbm > peak) peak = dbm;
    }
    delay(1);
  }
  readAndClearStats(edges, pulses);

  if (peak > binMaxRssi[index]) binMaxRssi[index] = peak;
  binBeckerPulses[index] += pulses;
  binEdges[index] += edges;

  if (peak >= HIT_THRESHOLD_DBM) {
    Serial.printf("HIT f=%.3f MHz rssi=%d dBm becker=%lu edges=%lu\n",
                  binFrequency(index) / 1e6, peak,
                  static_cast<unsigned long>(pulses),
                  static_cast<unsigned long>(edges));
  }

  scanIndex++;
  if (scanIndex >= SCAN_BINS) {
    scanIndex = 0;
    sweepCount++;
  }
}

// ---------- probe mode ----------
struct ProbeConfig {
  const char* name;
  uint8_t mdmcfg4;
  uint8_t mdmcfg3;
  uint8_t mdmcfg2;
  uint8_t deviatn;
};

static const ProbeConfig PROBE_CONFIGS[] = {
  {"2FSK 2.4k bw203 dev25", 0x86, 0x83, 0x00, 0x40},
  {"2FSK 5.6k bw325 dev25", 0x57, 0xC4, 0x06, 0x40},
  {"2FSK 2.4k bw203 dev48", 0x86, 0x83, 0x00, 0x47},
  {"2FSK 2.4k bw101 dev10", 0xC6, 0x83, 0x00, 0x27},
  {"OOK  2.4k bw203", 0x86, 0x83, 0x30, 0x40},
  {"OOK  5.6k bw325", 0x57, 0xC4, 0x30, 0x40},
};
static const size_t PROBE_COUNT =
    sizeof(PROBE_CONFIGS) / sizeof(PROBE_CONFIGS[0]);
static const uint32_t PROBE_DWELL_MS = 4000;

static size_t probeIndex = 0;
static double probeFrequencyHz = 0.0;

static void startProbe() {
  size_t best = 0;
  for (size_t i = 0; i < SCAN_BINS; i++) {
    if (binMaxRssi[i] > binMaxRssi[best]) best = i;
  }
  probeFrequencyHz =
      binMaxRssi[best] > -127 ? binFrequency(best) : 868282806.0;
  probeIndex = 0;
  Serial.printf("PROBE start f=%.3f MHz (press the remote repeatedly)\n",
                probeFrequencyHz / 1e6);
}

static void probeStep() {
  const ProbeConfig& config = PROBE_CONFIGS[probeIndex];
  if (!tune(probeFrequencyHz) ||
      !setModem(config.mdmcfg4, config.mdmcfg3, config.mdmcfg2,
                config.deviatn)) {
    Serial.println(F("PROBE ERROR: radio setup failed"));
    return;
  }
  delay(5);

  uint32_t edges = 0;
  uint32_t pulses = 0;
  readAndClearStats(edges, pulses);

  int peak = -127;
  const uint32_t until = millis() + PROBE_DWELL_MS;
  while (static_cast<int32_t>(until - millis()) > 0) {
    uint8_t raw = 0;
    if (readStatus(REG_RSSI, raw)) {
      const int dbm = rssiDbm(raw);
      if (dbm > peak) peak = dbm;
    }
    delay(5);
  }
  readAndClearStats(edges, pulses);

  uint8_t marc = 0xFF;
  uint8_t freqest = 0;
  readStatus(REG_MARCSTATE, marc);
  readStatus(REG_FREQEST, freqest);

  Serial.printf(
      "PROBE %-22s rssi=%d dBm becker=%lu edges=%lu marc=0x%02X freqest=%d\n",
      config.name, peak, static_cast<unsigned long>(pulses),
      static_cast<unsigned long>(edges), marc & 0x1F,
      static_cast<int>(static_cast<int8_t>(freqest)));

  probeIndex = (probeIndex + 1) % PROBE_COUNT;
}

// ---------- main ----------
enum class Mode : uint8_t { Scan, Probe };
static Mode mode = Mode::Scan;
static bool radioReady = false;

static void handleSerial() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    switch (command) {
      case 's':
        mode = Mode::Scan;
        scanIndex = 0;
        Serial.println(F("mode: SCAN"));
        break;
      case 'p':
        mode = Mode::Probe;
        startProbe();
        break;
      case 't':
        printTable();
        break;
      case 'z':
        resetStats();
        break;
      default:
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nBecker CC1101 frequency scanner"));
  Serial.println(F("commands: s=scan  p=probe  t=table  z=zero"));

  resetStats();
  radioReady = cc1101Init();
  if (!radioReady) {
    Serial.println(F("scanner disabled until a valid CC1101 is detected"));
    return;
  }

  pinMode(PIN_GDO2, INPUT);
  lastEdgeUs = micros();
  attachInterrupt(digitalPinToInterrupt(PIN_GDO2), onGdo2Edge, CHANGE);

  if (!setModem(SCAN_MDMCFG4, SCAN_MDMCFG3, SCAN_MDMCFG2, SCAN_DEVIATN)) {
    Serial.println(F("CC1101 ERROR: scan modem setup failed"));
  }
  Serial.println(F("SCAN running 867.700-869.100 MHz. Press the remote now."));
}

void loop() {
  handleSerial();
  if (!radioReady) {
    delay(500);
    return;
  }

  if (mode == Mode::Scan) {
    scanStep();
    static uint32_t lastTableMs = 0;
    if (millis() - lastTableMs >= 15000) {
      lastTableMs = millis();
      printTable();
    }
  } else {
    probeStep();
  }
}
