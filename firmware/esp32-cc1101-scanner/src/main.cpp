/**
 * Becker Centronic RF scanner for ESP32 + CC1101 (868 MHz).
 *
 * Version 2. The first scan showed a peak near 868.300-868.350 MHz at about
 * -48 dBm, but it reappeared identically on every sweep and produced zero GDO2
 * transitions, while a dozen bins reported an identical -59 dBm floor. That mix
 * of a permanent carrier and a contaminated floor cannot answer the question, so
 * this version measures each channel TWICE and reports the difference:
 *
 *   BASELINE - you do not touch the remote
 *   MEASURE  - you press the remote continuously
 *
 * Only a channel whose level rises between the two passes can be the remote.
 * RSSI is now sampled after a longer settle and averaged, and both mean and max
 * are kept, so a single transient sample can no longer create a fake peak.
 *
 * Wiring (unchanged):
 *   CC1101 VCC -> 3V3 (never 5V)   GND  -> GND
 *   MOSI -> GPIO23   SCLK -> GPIO18   MISO -> GPIO19
 *   GDO2 -> GPIO4    GDO0 -> not connected   CSN -> GPIO5
 *
 * Serial: 115200 baud. One character + Enter:
 *   b  baseline pass (do not press)     m  measure pass (keep pressing)
 *   t  print the comparison table       p  probe modulations on the best channel
 *   z  clear everything
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

static void takeStats(uint32_t& edges, uint32_t& beckerPulses) {
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
  return strobe(STROBE_SRX);
}

static bool setModem(uint8_t mdmcfg4, uint8_t mdmcfg3, uint8_t mdmcfg2,
                     uint8_t deviatn) {
  if (!strobe(STROBE_SIDLE)) return false;
  delayMicroseconds(200);
  if (!writeReg(0x10, mdmcfg4)) return false;
  if (!writeReg(0x11, mdmcfg3)) return false;
  if (!writeReg(0x12, mdmcfg2)) return false;
  if (!writeReg(0x15, deviatn)) return false;
  return strobe(STROBE_SRX);
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

// ---------- channel plan ----------
static const double SCAN_START_HZ = 867700000.0;
static const double SCAN_STEP_HZ = 50000.0;
static const size_t SCAN_BINS = 29;   // 867.700 .. 869.100 MHz
// Narrow filter (101.6 kHz) so a carrier only registers near its real channel.
static const uint8_t SCAN_MDMCFG4 = 0xC6;
static const uint8_t SCAN_MDMCFG3 = 0x83;   // about 2400 baud
static const uint8_t SCAN_MDMCFG2 = 0x00;   // 2-FSK, no sync gating
static const uint8_t SCAN_DEVIATN = 0x40;   // about 25 kHz

// RSSI needs the AGC to settle after each channel change; sample only after it
// and average, instead of trusting one early reading.
static const uint32_t RSSI_SETTLE_MS = 6;
static const uint32_t RSSI_SAMPLES = 10;
static const uint32_t RSSI_SAMPLE_GAP_MS = 2;

struct BinStats {
  int32_t meanSum;
  uint32_t meanCount;
  int maxDbm;
  uint32_t edges;
  uint32_t beckerPulses;
};

static BinStats baseline[SCAN_BINS];
static BinStats measured[SCAN_BINS];
static uint32_t baselineSweeps = 0;
static uint32_t measureSweeps = 0;
static size_t scanIndex = 0;

static double binFrequency(size_t index) {
  return SCAN_START_HZ + static_cast<double>(index) * SCAN_STEP_HZ;
}

static void clearStats(BinStats* stats) {
  for (size_t i = 0; i < SCAN_BINS; i++) {
    stats[i].meanSum = 0;
    stats[i].meanCount = 0;
    stats[i].maxDbm = -127;
    stats[i].edges = 0;
    stats[i].beckerPulses = 0;
  }
}

static int binMean(const BinStats& stats) {
  if (stats.meanCount == 0) return -127;
  return static_cast<int>(stats.meanSum / static_cast<int32_t>(stats.meanCount));
}

// ---------- one measurement of one channel ----------
static bool sampleBin(size_t index, BinStats& into) {
  if (!tune(binFrequency(index))) return false;
  delay(RSSI_SETTLE_MS);

  uint32_t edges = 0;
  uint32_t pulses = 0;
  takeStats(edges, pulses);

  int32_t sum = 0;
  uint32_t count = 0;
  int peak = -127;
  for (uint32_t i = 0; i < RSSI_SAMPLES; i++) {
    uint8_t raw = 0;
    if (readStatus(REG_RSSI, raw)) {
      const int dbm = rssiDbm(raw);
      sum += dbm;
      count++;
      if (dbm > peak) peak = dbm;
    }
    delay(RSSI_SAMPLE_GAP_MS);
  }
  takeStats(edges, pulses);

  if (count > 0) {
    into.meanSum += sum;
    into.meanCount += count;
  }
  if (peak > into.maxDbm) into.maxDbm = peak;
  into.edges += edges;
  into.beckerPulses += pulses;
  return true;
}

// ---------- table ----------
static size_t bestDeltaBin() {
  size_t best = 0;
  int bestDelta = -1000;
  for (size_t i = 0; i < SCAN_BINS; i++) {
    if (measured[i].meanCount == 0) continue;
    const int base = baseline[i].meanCount == 0 ? -127 : binMean(baseline[i]);
    const int delta = measured[i].maxDbm - base;
    if (delta > bestDelta) {
      bestDelta = delta;
      best = i;
    }
  }
  return best;
}

static void printTable() {
  Serial.printf("TABLE baseline_sweeps=%lu measure_sweeps=%lu\n",
                static_cast<unsigned long>(baselineSweeps),
                static_cast<unsigned long>(measureSweeps));
  if (measureSweeps == 0) {
    Serial.println(F("no measure pass yet: send b (idle), then m (pressing)"));
  }
  const size_t best = bestDeltaBin();
  for (size_t i = 0; i < SCAN_BINS; i++) {
    const int baseMean = binMean(baseline[i]);
    const int baseMax = baseline[i].maxDbm;
    const int measMean = binMean(measured[i]);
    const int measMax = measured[i].maxDbm;
    const int delta = (measMax == -127 || baseMean == -127)
                          ? 0
                          : measMax - baseMean;
    Serial.printf(
        "BIN f=%.3f idle_mean=%d idle_max=%d press_mean=%d press_max=%d "
        "delta=%d becker=%lu edges=%lu%s%s\n",
        binFrequency(i) / 1e6, baseMean, baseMax, measMean, measMax, delta,
        static_cast<unsigned long>(measured[i].beckerPulses),
        static_cast<unsigned long>(measured[i].edges),
        i == best && measureSweeps > 0 ? "  <== best delta" : "",
        baseMax >= -65 ? "  [carrier already present when idle]" : "");
  }
  if (measureSweeps > 0) {
    Serial.printf("BEST f=%.3f MHz delta=%d dB\n", binFrequency(best) / 1e6,
                  measured[best].maxDbm - binMean(baseline[best]));
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
  const size_t best = bestDeltaBin();
  probeFrequencyHz =
      measureSweeps > 0 ? binFrequency(best) : 868282806.0;
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
  delay(RSSI_SETTLE_MS);

  uint32_t edges = 0;
  uint32_t pulses = 0;
  takeStats(edges, pulses);

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
  takeStats(edges, pulses);

  uint8_t marc = 0xFF;
  uint8_t freqest = 0;
  readStatus(REG_MARCSTATE, marc);
  readStatus(REG_FREQEST, freqest);

  Serial.printf(
      "PROBE %-22s max_rssi=%d becker=%lu edges=%lu marc=0x%02X freqest=%d\n",
      config.name, peak, static_cast<unsigned long>(pulses),
      static_cast<unsigned long>(edges), marc & 0x1F,
      static_cast<int>(static_cast<int8_t>(freqest)));

  probeIndex = (probeIndex + 1) % PROBE_COUNT;
}

// ---------- main ----------
enum class Mode : uint8_t { Idle, Baseline, Measure, Probe };
static Mode mode = Mode::Idle;
static bool radioReady = false;

static void handleSerial() {
  while (Serial.available() > 0) {
    switch (static_cast<char>(Serial.read())) {
      case 'b':
        clearStats(baseline);
        baselineSweeps = 0;
        scanIndex = 0;
        mode = Mode::Baseline;
        Serial.println(F("BASELINE running: do NOT touch the remote"));
        break;
      case 'm':
        clearStats(measured);
        measureSweeps = 0;
        scanIndex = 0;
        mode = Mode::Measure;
        Serial.println(F("MEASURE running: press the remote continuously"));
        break;
      case 'p':
        mode = Mode::Probe;
        startProbe();
        break;
      case 't':
        printTable();
        break;
      case 'z':
        clearStats(baseline);
        clearStats(measured);
        baselineSweeps = 0;
        measureSweeps = 0;
        mode = Mode::Idle;
        Serial.println(F("cleared"));
        break;
      default:
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nBecker CC1101 scanner v2 (idle vs press comparison)"));
  Serial.println(F("commands: b=baseline  m=measure  t=table  p=probe  z=clear"));

  clearStats(baseline);
  clearStats(measured);

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
  Serial.println(F("ready. send b and stay idle ~20 s, then m and keep pressing"));
}

void loop() {
  handleSerial();
  if (!radioReady) {
    delay(500);
    return;
  }

  if (mode == Mode::Baseline || mode == Mode::Measure) {
    BinStats* target = mode == Mode::Baseline ? baseline : measured;
    if (!sampleBin(scanIndex, target[scanIndex])) {
      Serial.println(F("SCAN ERROR: tune failed"));
      return;
    }
    scanIndex++;
    if (scanIndex >= SCAN_BINS) {
      scanIndex = 0;
      if (mode == Mode::Baseline) {
        baselineSweeps++;
        Serial.printf("baseline sweep %lu done\n",
                      static_cast<unsigned long>(baselineSweeps));
      } else {
        measureSweeps++;
        Serial.printf("measure sweep %lu done\n",
                      static_cast<unsigned long>(measureSweeps));
      }
    }
  } else if (mode == Mode::Probe) {
    probeStep();
  } else {
    delay(50);
  }
}
