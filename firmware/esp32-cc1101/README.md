# ESP32 + CC1101 Becker raw capture

Capture the demodulated Becker Centronic signal at 868.283 MHz (2-FSK) with
an ESP32 and an 868 MHz CC1101 module.

The Becker/SIGNALduino register set uses the CC1101 **asynchronous serial
mode**. Receive data is exposed on **GDO2** and is not available through the RX
FIFO. This firmware records GDO2 edge timings and prints SIGNALduino-style
signed pulse durations for later Manchester decoding.

## Hardware

- ESP32 DevKit / `esp32dev`
- CC1101 module tuned for **868 MHz** (not the common 433 MHz version)
- 868 MHz antenna

The CC1101 is a **3.3 V-only** device. Never connect it to 5 V.

### Alexis' 2.0 mm-pitch module

Hold the module with components facing you, antenna holes at the top. The eight
bottom pads are, from left to right:

| Pad | Signal | ESP32 |
|----:|--------|-------|
| 1 | VCC | 3V3 |
| 2 | GND | GND |
| 3 | MOSI / SI | GPIO23 |
| 4 | SCLK | GPIO18 |
| 5 | MISO / SO | GPIO19 |
| 6 | **GDO2** | **GPIO4** |
| 7 | GDO0 | Not connected |
| 8 | CSN | GPIO5 |

The module pads use a **2.0 mm pitch**, so a continuous 2.54 mm breadboard
header does not fit directly. Use a 2.0-to-2.54 mm adapter or individual wires.
The centre antenna hole is `ANT`; the two outer holes are ground.

> **Important:** the PNG wiring diagrams currently present in this folder show
> an earlier generic 2.54 mm module and the obsolete GDO0/GPIO2 capture path.
> Do not use them for this module. Follow the table above.

Keep SPI wires short (ideally below 15 cm).

## Build and flash

1. Optionally set `WIFI_SSID` and `WIFI_PASS` in `src/main.cpp`. Leave them as
   `CHANGE_ME` for serial-only capture.
2. Disconnect the CC1101 while flashing the ESP32 for the first time.
3. Run:

   ```bash
   pio run -t upload
   pio device monitor
   ```

4. Power off, wire the CC1101 according to the table, then reconnect USB.

The serial monitor runs at 115200 baud. A valid module reports an identity such
as:

```text
CC1101 identity: PARTNUM=0x00 VERSION=0x14
CC1101 ready: 868.283 MHz 2-FSK, async data on GDO2/GPIO4
```

`VERSION=0x04` is also common. `0x00` or `0xFF` means the module was not
identified; check power and SPI wiring. Capture remains disabled in that case,
so an absent module can no longer create fake `ff` frames.

## Capture and frame-length diagnostic

The firmware prints a radio heartbeat every two seconds:

```text
DIAG edges=0 delta=0/2s gdo2=1 marc=0x0D rssi=-93dBm pkt=0x00
```

`marc=0x0D` means that the CC1101 is in RX. Pressing a remote button should make
`edges` and `delta` increase even when the pulse train does not yet match the
expected Becker timing.

Every burst containing at least two pulse durations is printed without a timing
filter:

```text
BURST n=96 total_us=54180 match=98% c=417 halfbits=130 bits_est=65.0 D=-403,432,-826,846,...
```

- `n`: number of captured level durations
- `total_us`: observed burst duration
- `match`: percentage of durations matching the expected 417/834 us timing
- `c`: estimated Manchester half-clock in microseconds
- `halfbits`: estimated count of Manchester half-bits
- `bits_est`: direct frame-length estimate (`halfbits / 2`)
- negative duration: low level; positive duration: high level

`BURST_OVERFLOW` means more than 512 durations were seen without a frame gap.
The complete timing sequence is kept so the framing assumptions can be checked
instead of silently rejecting an unexpected but potentially valid signal.

If Wi-Fi credentials are configured, the ESP32 also exposes:

- `GET /frames` — stored raw captures
- `POST /clear` — clear stored captures

Wi-Fi is optional, uses reduced transmit power, and times out after 15 seconds;
serial capture continues if Wi-Fi is unavailable.

## Current scope

This firmware is **capture-only**. Transmission is intentionally disabled until
real captures establish the exact 65/66-bit on-air format. A CC1101 cannot
produce valid future encrypted rolling codes without the required key and frame
construction.

The register set comes from
[centronic-py TECHNICAL.md](https://github.com/ole1986/centronic-py/blob/master/TECHNICAL.md)
and its FHEM/SIGNALduino Becker configuration.
