# Becker CC1101 scanner (idle vs press)

Diagnostic firmware for the state we are in: with the published Becker profile
(868.282806 MHz, 2-FSK) the remote is received strongly, `MARCSTATE` is `0x0D`
and `FREQEST` is `0`, yet GDO2 carries no data transitions.

## Why version 2 exists

The first scan reported a peak at 868.300-868.350 MHz around -48 dBm, but:

- the same `HIT` lines reappeared on every sweep, which looks like a permanent
  carrier rather than bursts caused by button presses;
- twelve channels reported an identical -59 dBm floor, while the idle noise floor
  measured earlier at 325 kHz bandwidth was about -81 dBm;
- the peak channel produced `edges=0`, so nothing was modulating there.

A single-pass maximum cannot separate the remote from a permanent neighbour
signal or from an RSSI reading taken before the AGC settled. This version
therefore measures every channel twice and reports the difference.

## Wiring

Unchanged.

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

## Procedure

| Step | Command | What you do |
|------|---------|-------------|
| 1 | `b` | Do **not** touch the remote for about 20 seconds |
| 2 | `m` | Press the remote continuously for about 30 seconds |
| 3 | `t` | Send the table |
| 4 | `p` | Keep pressing while modulation candidates are tried |

Each pass prints `baseline sweep N done` / `measure sweep N done` so you know when
enough data is collected. Three sweeps per pass is plenty.

## Reading the table

```text
BIN f=868.300 idle_mean=-92 idle_max=-90 press_mean=-60 press_max=-47 delta=45 becker=131 edges=268  <== best delta
```

- `delta` is what matters: `press_max` minus `idle_mean`. Only a channel that
  rises when you press can be the remote.
- `[carrier already present when idle]` marks channels that are strong even
  without pressing. Those are neighbours or internal spurs, not the remote.
- `becker` counts pulses between 250 and 1150 us, the Becker range (half-bit
  about 414 us, full bit about 828 us). A non-zero count on the best-delta
  channel is the real target.

RSSI is now sampled after a 6 ms settle, ten times per channel, and both the mean
and the maximum are kept.

## Step 2 - identify the modulation

`p` stays on the best-delta channel and cycles candidates, 4 seconds each:

```text
PROBE 2FSK 2.4k bw203 dev25   max_rssi=-47 becker=0   edges=12  marc=0x0D freqest=0
PROBE OOK  2.4k bw203         max_rssi=-47 becker=131 edges=268 marc=0x0D freqest=0
```

The configuration with a clearly higher `becker` count is the real modulation. If
an OOK entry wins, this remote is not 2-FSK like the documented CC31/CC51
reference, which changes the receive strategy for the whole project.

## Scope

Diagnostic only: it does not decode frames and never transmits. Recovering the
KeeLoq key remains out of reach without a device dump.
