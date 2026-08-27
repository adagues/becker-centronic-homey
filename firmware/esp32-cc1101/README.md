# ESP32 + CC1101 Becker bridge

Capture and transmit Becker Centronic frames (868.283 MHz, 2-FSK). Used to
calibrate the Homey app's signal definition — and as the permanent Wi-Fi
bridge if Homey's own radio turns out unable to transmit FSK.

## ⚠️ Buy the right module

CC1101 boards are **band-specific**: the antenna matching network is tuned at
the factory. Order a module explicitly sold as **868 MHz** — the common
433 MHz boards work poorly to not at all at 868. Any ESP32 dev board works.

## Wiring

| CC1101 | ESP32 |
|--------|-------|
| VCC    | 3V3 (**never 5V**) |
| GND    | GND |
| SCK    | GPIO18 |
| MISO   | GPIO19 |
| MOSI   | GPIO23 |
| CSn    | GPIO5 |
| GDO0   | GPIO2 |

## Usage

1. Set `WIFI_SSID` / `WIFI_PASS` in `src/main.cpp`
2. `pio run -t upload && pio device monitor`
3. Press buttons on your Becker remote near the module
4. `curl http://<esp32-ip>/frames` → captured frames as hex
5. Share the captures — they drive the calibration of the Homey signal
   definition (and validate the frame builder in `lib/BeckerProtocol.js`)

Transmit test (once capture confirms the format):

```bash
curl -X POST "http://<esp32-ip>/tx?hex=<frame-hex>"
```

## Register set

The CC1101 registers in `src/main.cpp` are verbatim from the
[centronic-py TECHNICAL.md](https://github.com/ole1986/centronic-py/blob/master/TECHNICAL.md)
FHEM/SIGNALduino configuration (2-FSK, 868.283 MHz).

## Status

**Untested on hardware.** Packet-length / sync-word handling may need
adjustment after the first real captures — that is expected and part of the
calibration loop.
