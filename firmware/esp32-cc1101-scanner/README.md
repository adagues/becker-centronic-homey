# Becker CC1101 frequency scanner

Diagnostic firmware for the case we are in: with the published Becker profile
(868.282806 MHz, 2-FSK) the remote is received at about -37 dBm, `MARCSTATE` is
`0x0D` (RX) and `FREQEST` is `0`, yet GDO2 produces no data transitions. That
combination fits a carrier sitting inside the wide 325 kHz filter but off the
tuned centre, or a modulation that is not 2-FSK.

## Wiring

Unchanged from the capture firmware.

| CC1101 | ESP32 |
|--------|-------|
| VCC | 3V3 (**never 5V**) |
| GND | GND |
| MOSI | GPIO23 |
| SCLK | GPIO18 |
| MISO | GPIO19 |
| **GDO2** | **GPIO4** |
| GDO0 | not connected |
| CSN | GPIO5 |

## Build and flash

```bash
pio run -t clean
pio run -t upload
pio device monitor -b 115200
```

Wi-Fi is not used at all: it caused brownout resets on this board.

## Serial commands

One character followed by Enter.

| Command | Effect |
|---------|--------|
| `s` | scan mode (default) |
| `p` | probe mode on the strongest frequency found |
| `t` | print the scan table now |
| `z` | clear statistics |

## Step 1 - locate the carrier

Scan mode sweeps 867.700 to 869.100 MHz in 50 kHz steps with a narrow 101.5 kHz
filter, so a strong carrier only registers near its real channel.

Press the remote repeatedly for 30 to 60 seconds. Each burst that exceeds
-65 dBm prints immediately:

```text
HIT f=868.300 MHz rssi=-38 dBm becker=0 edges=54
```

A table is printed every 15 seconds:

```text
BIN f=868.300 MHz max_rssi=-38 dBm becker=0 edges=54  <== strongest
BEST f=868.300 MHz max_rssi=-38 dBm
```

Reading it:

- `max_rssi` peaks at the remote's actual transmit frequency.
- a peak away from 868.283 MHz means our tuned centre was wrong.
- `becker` counts pulses between 250 and 1150 us, the Becker range (half-bit
  about 414 us, full bit about 828 us). Any non-zero value is a strong lead.

## Step 2 - identify the modulation

Send `p`. The firmware stays on the strongest frequency and cycles candidates,
4 seconds each, while you keep pressing the remote:

```text
PROBE 2FSK 2.4k bw203 dev25   rssi=-38 dBm becker=0   edges=12  marc=0x0D freqest=0
PROBE OOK  2.4k bw203         rssi=-38 dBm becker=131 edges=268 marc=0x0D freqest=0
```

The configuration with a clearly higher `becker` count is the real modulation.
If OOK wins, this remote is not 2-FSK like the documented CC31/CC51 reference,
which changes the receive and replay strategy for the whole project.

## Scope

Diagnostic only. It does not decode frames and does not transmit. Recovering the
KeeLoq key remains out of reach without a device dump.
