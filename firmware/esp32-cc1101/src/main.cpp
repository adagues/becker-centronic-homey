/**
 * Becker Centronic ESP32 + CC1101 bridge.
 *
 * Two jobs:
 *  1. CAPTURE: listen on 868.283 MHz (2-FSK, Becker register set from
 *     centronic-py TECHNICAL.md / FHEM SIGNALduino) and expose received
 *     frames as hex over serial and GET /frames.
 *  2. BRIDGE: accept raw frames via POST /tx and transmit them — the
 *     permanent fallback if Homey's own radio cannot send FSK.
 *
 * Wiring (VSPI defaults, changeable below):
 *   CC1101   ESP32
 *   VCC   -> 3V3   (NEVER 5V)
 *   GND   -> GND
 *   SCK   -> GPIO18
 *   MISO  -> GPIO19
 *   MOSI  -> GPIO23
 *   CSn   -> GPIO5
 *   GDO0  -> GPIO2
 *
 * Status: UNTESTED ON HARDWARE — starting point for calibration.
 */

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------- configuration ----------
static const char* WIFI_SSID = "CHANGE_ME";
static const char* WIFI_PASS = "CHANGE_ME";

static const int PIN_CS = 5;
static const int PIN_GDO0 = 2;

// ---------- CC1101 low-level ----------
static const uint8_t STROBE_SRES = 0x30;
static const uint8_t STROBE_SRX = 0x34;
static const uint8_t STROBE_STX = 0x35;
static const uint8_t STROBE_SIDLE = 0x36;
static const uint8_t STROBE_SFRX = 0x3A;
static const uint8_t STROBE_SFTX = 0x3B;
static const uint8_t REG_RXBYTES = 0x3B;
static const uint8_t REG_FIFO = 0x3F;

// Becker Centronic register set — verbatim from centronic-py TECHNICAL.md
// (FHEM SIGNALduino cc1101_reg command): 2-FSK @ 868.283 MHz.
// Format: {address, value}
static const uint8_t BECKER_REGS[][2] = {
  {0x00, 0x0D}, {0x01, 0x2E}, {0x02, 0x2D}, {0x03, 0x47}, {0x04, 0xD3},
  {0x05, 0x91}, {0x06, 0x3D}, {0x07, 0x04}, {0x08, 0x32}, {0x09, 0x00},
  {0x0A, 0x00}, {0x0B, 0x06}, {0x0C, 0x00}, {0x0D, 0x21}, {0x0E, 0x65},
  {0x0F, 0x3F}, {0x10, 0x57}, {0x11, 0xC4}, {0x12, 0x06}, {0x13, 0x23},
  {0x14, 0xB9}, {0x15, 0x40}, {0x16, 0x07}, {0x17, 0x00}, {0x18, 0x18},
  {0x19, 0x14}, {0x1A, 0x6C}, {0x1B, 0x00}, {0x1C, 0x00}, {0x1D, 0x92},
  {0x1E, 0x87}, {0x1F, 0x6B}, {0x20, 0xF8}, {0x21, 0xB6}, {0x22, 0x11},
  {0x23, 0xEF}, {0x24, 0x2B}, {0x25, 0x14}, {0x26, 0x1F}, {0x27, 0x41},
  {0x28, 0x00},
};

static uint8_t spiTransfer(uint8_t addr, uint8_t value) {
  digitalWrite(PIN_CS, LOW);
  while (digitalRead(19)) {} // wait for MISO low = chip ready
  SPI.transfer(addr);
  uint8_t result = SPI.transfer(value);
  digitalWrite(PIN_CS, HIGH);
  return result;
}

static void strobe(uint8_t cmd) {
  digitalWrite(PIN_CS, LOW);
  while (digitalRead(19)) {}
  SPI.transfer(cmd);
  digitalWrite(PIN_CS, HIGH);
}

static void writeReg(uint8_t addr, uint8_t value) { spiTransfer(addr, value); }
static uint8_t readReg(uint8_t addr) { return spiTransfer(addr | 0xC0, 0x00); }

static void cc1101Init() {
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  SPI.begin();
  delay(10);
  strobe(STROBE_SRES);
  delay(10);
  for (auto& reg : BECKER_REGS) writeReg(reg[0], reg[1]);
  strobe(STROBE_SFRX);
  strobe(STROBE_SRX);
}

// ---------- frame buffer ----------
static const int MAX_FRAMES = 32;
static String frames[MAX_FRAMES];
static int frameCount = 0;

static void storeFrame(const String& hex) {
  if (frameCount < MAX_FRAMES) {
    frames[frameCount++] = hex;
  } else {
    for (int i = 1; i < MAX_FRAMES; i++) frames[i - 1] = frames[i];
    frames[MAX_FRAMES - 1] = hex;
  }
  Serial.println("RX " + hex);
}

static void pollReceive() {
  uint8_t n = readReg(REG_RXBYTES) & 0x7F;
  if (n == 0) return;
  String hex;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t b = spiTransfer(REG_FIFO | 0xC0, 0x00);
    if (b < 0x10) hex += '0';
    hex += String(b, HEX);
  }
  storeFrame(hex);
  strobe(STROBE_SFRX);
  strobe(STROBE_SRX);
}

// ---------- transmit ----------
static bool transmitHex(const String& hex) {
  if (hex.length() % 2 != 0 || hex.length() == 0 || hex.length() > 128) return false;
  strobe(STROBE_SIDLE);
  strobe(STROBE_SFTX);
  digitalWrite(PIN_CS, LOW);
  while (digitalRead(19)) {}
  SPI.transfer(REG_FIFO | 0x40); // burst write to TX FIFO
  for (unsigned i = 0; i < hex.length(); i += 2) {
    SPI.transfer(strtoul(hex.substring(i, i + 2).c_str(), nullptr, 16));
  }
  digitalWrite(PIN_CS, HIGH);
  strobe(STROBE_STX);
  delay(50);
  strobe(STROBE_SIDLE);
  strobe(STROBE_SRX);
  return true;
}

// ---------- HTTP ----------
static WebServer server(80);

void setup() {
  Serial.begin(115200);
  pinMode(PIN_GDO0, INPUT);
  cc1101Init();
  Serial.println("CC1101 initialized with Becker register set (868.283 MHz 2-FSK)");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain",
      "Becker CC1101 bridge\nGET /frames — captured frames\nPOST /tx?hex=... — transmit raw frame");
  });
  server.on("/frames", HTTP_GET, []() {
    String out;
    for (int i = 0; i < frameCount; i++) out += frames[i] + "\n";
    server.send(200, "text/plain", out);
  });
  server.on("/tx", HTTP_POST, []() {
    String hex = server.arg("hex");
    if (transmitHex(hex)) server.send(200, "text/plain", "sent " + hex);
    else server.send(400, "text/plain", "invalid hex payload");
  });
  server.begin();
}

void loop() {
  server.handleClient();
  pollReceive();
}
