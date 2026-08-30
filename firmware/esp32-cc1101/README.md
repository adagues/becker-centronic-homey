# ESP32 + CC1101 Becker raw capture

Capture the demodulated Becker Centronic signal at 868.283 MHz (2-FSK) with
an ESP32 and an 868 MHz CC1101 module.

The Becker/SIGNALduino register set uses the CC1101 **asynchronous serial
mode**. Receive data is exposed on **GDO2** and is not available through the RX
FIFO. This firmware records GDO2 edge timings and decodes Manchester frames.

## Measured centre frequency: 868.350 MHz

The published Becker profile uses 868.282806 MHz, but a scanner sweep of this
remote (`firmware/esp32-cc1101-scanner`, idle-vs-press comparison) put the
carrier 40-65 kHz higher: idle floor -108 dBm everywhere, and while transmitting
868.300/868.350 MHz rose to -45/-44 dBm, a 64 dB delta with no permanent carrier
anywhere. At 868.283 MHz both FSK tones therefore sat on the same side of the
discriminator, GDO2 stayed static and nothing could be decoded.

A probe pass at 868.350 MHz then produced thousands of Becker-range pulses for
every 2-FSK candidate and none at all for OOK, so this firmware now uses:

- centre 868.349854 MHz (`FREQ2/1/0 = 0x21 0x65 0xE8`)
- 2-FSK, deviation 25.4 kHz, no sync-word gating (`MDMCFG2 = 0x00`)
- 203 kHz receive filter, 2399 baud (`MDMCFG4 = 0x86`, `MDMCFG3 = 0x83`)
- `MCSM1 = 0x00`, matching the proven SIGNALduino profile
- no RX FIFO polling during capture

## Serial commands

| Key | Effect |
|-----|--------|
| `+` | retune 10 kHz up |
| `-` | retune 10 kHz down |
| `i` | print current frequency and counters |
| `r` | re-arm the receive path |

Use `+`/`-` to trim the centre without reflashing if frames decode partially.

`DIAG` reports `peak_rssi`, the strongest reading seen during the whole two-second
window, not a single instantaneous sample. A remote burst lasts only tens of
milliseconds, so a once-per-report reading would almost always miss it. RSSI
sampling is skipped while a pulse train is active, so SPI traffic cannot disturb
the capture.

While transmitting a few centimetres from the antenna, `peak_rssi` should reach
roughly -40 dBm. If it never rises above about -100 dBm, the remote did not
transmit in that window: press for about a second, release, and repeat.

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
DIAG edges=0 delta=0/2s gdo2=1 marc=0x0D rssi=-93dBm pkt=0x00 mc=0 reject=0 drop=0 glitches=0
```

`marc=0x0D` means that the CC1101 is in RX. The firmware configures MCSM1 to
remain in RX, silently drains bytes that the asynchronous configuration still
accumulates in RXFIFO, and recovers from any state stuck outside RX. Recovery
waits for `marc=0x01` (IDLE) before issuing `SFRX`, because the CC1101 ignores
that strobe in RX/RX_RST, then waits for `marc=0x0D` after `SRX`. Pressing a remote button should make `edges` and `delta`
increase even when the pulse train does not yet match the expected Becker
timing.

The ISR stream is consumed continuously in 64-pulse chunks; it no longer waits
for a silent RF gap and therefore cannot grow into a merged 512-pulse burst.
A sliding Manchester detector:

1. keeps contiguous pulse runs near 417 us (one half-bit) or 834 us (two
   half-bits);
2. expands each run to half-bit levels;
3. tests both possible Manchester phases;
4. extracts the longest contiguous sequence of valid opposite-level pairs;
5. reports only sequences containing at least 40 coherent bits.

A detected frame is printed as:

```text
MC bits=65 c=417 pulses=96 halfbits=130 glitches=4 phase=0 hex=FFE9BC20B299FC858 inv=001643DF4D66037A0 D=-403,432,-826,846,...
```

- `bits`: exact length of the longest coherent Manchester sequence
- `c`: estimated half-bit clock in microseconds
- `pulses` / `halfbits`: timing-run sizes before Manchester extraction
- `glitches`: narrow opposite-level pulses merged before timing classification
- `phase`: selected half-bit pairing offset (0 or 1)
- `hex`: decoded polarity candidate, padded on the right to a full nibble
- `inv`: inverse-polarity candidate
- `D`: signed raw durations for independent checking

The periodic `DIAG` line includes `mc`, `reject`, `drop`, and `glitches`:
detected frames, timing-compatible runs rejected by Manchester validation, ISR
chunk overflows, and cumulative narrow pulses merged by the deglitcher. Continuous asynchronous RF noise should raise `edges` and possibly
`reject`, but must not produce `MC` frames unless it contains a coherent run.

Before timing classification, an `A - short opposite level - A` sequence
whose middle interval is below 220 us is merged into one stable pulse. The ISR
keeps intervals from 20 us upward so those glitches remain reconstructable.

The detector itself is hardware-independent and covered by a host test with
synthetic 65-bit and 66-bit frames, polarity inversion, timing jitter, injected
90 us glitches, noise boundaries, and timing-compatible non-Manchester noise.

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
