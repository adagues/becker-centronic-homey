# Ralf9 SIGNALduino — Becker control build

This is an isolated, receive-only control build derived from Ralf9 SIGNALduino.
Its purpose is to reproduce the receiver that produced the documented Becker
Centronic capture before making further changes to the custom firmware.

## Provenance and license

- Upstream: <https://github.com/Ralf9/SIGNALDuino>
- Upstream branch: `dev-r422_cc1101`
- Pinned upstream commit: `2fc2c8a718cd232ca8ce24c426784e720fcc7167`
- Upstream version string: `4.2.2-dev220712`
- License: GNU GPL-3.0; see `LICENSE`

The vendored upstream files remain GPL-3.0. Local changes are intentionally
limited to board pins, serial-only operation, and the compiled Becker profile.

## Wiring

Hold Alexis' CC1101 with components visible and antenna holes at the top. The
bottom pads are numbered left-to-right.

| CC1101 pad | Signal | ESP32 |
|---:|---|---|
| 1 | VCC | 3V3 only |
| 2 | GND | GND |
| 3 | MOSI | GPIO23 |
| 4 | SCLK | GPIO18 |
| 5 | MISO | GPIO19 |
| 6 | GDO2 raw data | GPIO4 |
| 7 | GDO0 | not connected |
| 8 | CSN | GPIO5 |

Antenna wire goes to the centre `ANT` hole. The outer antenna holes are ground.

## Becker profile

The build always writes the reviewed profile directly, bypassing stale EEPROM
register banks. At startup it also verifies the complete Ralf9 default
configuration and enforces radio B, bank 0, `ccmode=0`, RX and Manchester
decoding. EEPROM is rewritten only when a value differs, so flash is not
rewritten on every boot.

This matters twice over:

1. A never-initialised EEPROM leaves decoder limits at `0xFF`. In particular
   `maxnumpat=255` overflows the fixed 16-entry pattern arrays. The value is
   also clamped defensively at load time.
2. Upstream pins the old ESP32 core, where AVR-style `cli()`/`sei()` inside the
   GPIO ISR were harmless. On a modern core they map to
   `portDISABLE_INTERRUPTS()`/`portENABLE_INTERRUPTS()`, which must not be
   called from ISR context and reboot the chip as soon as GDO2 produces edges.
   They are compiled out in this control build; the ISR only pushes a single
   16-bit value into a single-producer FIFO.

Both faults appeared as a fast reboot loop right after `rxB=1`.

- 868.282806 MHz
- 2-FSK
- 325 kHz receive bandwidth
- 5.604 kbit/s configured data rate
- 25.391 kHz deviation
- asynchronous serial output on GDO2
- SlowRF/raw `ccmode=0`
- `MCSM1=0x00`, matching the published Ralf9 configuration

Wi-Fi is disabled at runtime. All SIGNALduino messages and commands use USB
serial at 115200 baud.

## Build and flash

Open this directory as a PlatformIO project, then run:

```bash
pio run -e esp32_becker_serial -t clean
pio run -e esp32_becker_serial -t upload
pio device monitor -b 115200
```

Keep the CC1101 wiring unchanged from the custom firmware. Press a Becker remote
button near the antenna. The reference output format is:

```text
MC;LL=-809;LH=844;SL=-390;SH=443;D=CA5E62DB2542C1DB8;C=414;L=66;R=202;
```

A result near `C=414` and `L=65` or `L=66` confirms the RF hardware and the
published register profile independently of the custom decoder.

## Scope

This is a diagnostic control, not the permanent Homey bridge. It does not
recover the KeeLoq key and cannot generate future encrypted rolling codes.
