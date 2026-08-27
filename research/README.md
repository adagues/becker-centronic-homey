# Becker Centronic RF cryptanalysis

## Goal

Recover the unpublished transformation between the 21-byte logical frame sent
to Becker's USB stick and the 65/66-bit Manchester-demodulated RF payload.
Without this transformation, transmitting the logical USB frame directly can
never work.

## Confirmed public data

Ole1986 published one chosen-input pair:

- logical: `0000000002010B00000000001737C0210101004084`
- RF: `CA5E62DB2542C1DB8`, 66 valid bits, Manchester clock ≈414 µs
- fields: counter=0, unit=`1737C`, channel=1, command=DOWN (`40`)

Source: <https://forum.fhem.de/index.php/topic,110043.msg1040546.html>

RF parameters from the same source:

- 868.283 MHz
- 2-FSK, deviation 40 kHz
- Manchester timing: short low -390 µs, short high 443 µs, long low
  -809 µs, long high 844 µs (capture-specific jitter)

## Strong hypothesis: KeeLoq-like 66-bit frame

Classic KeeLoq/HCS301 transmissions are 66 bits: a 32-bit encrypted hopping
code plus 34 fixed/function/status bits. That shape matches Becker's 66-bit RF
capture exceptionally well, and KeeLoq implementations exist over FSK 868 MHz.
This is a hypothesis, not yet proof: no Becker source found publicly names
KeeLoq.

If confirmed, merely learning pulse timings is insufficient. We must identify:

1. field ordering, bit polarity and padding/alignment;
2. how Becker maps unit/channel/command into the fixed 34 bits;
3. the key-derivation/encryption scheme used for the 32-bit hopping code.

A manufacturer key cannot realistically be brute-forced at 64 bits. Practical
routes are identifying the encoder IC/firmware, deriving the key-generation
scheme from several physical remotes, or using the official USB stick as the
RF bridge instead of reimplementing encryption.

## Minimum useful capture corpus

With an ESP32+CC1101 or SDR, capture **short presses**, not long holds. Keep raw
IQ/FIFO data and decoded bits. Record at least:

| Set | Captures | Purpose |
|-----|----------|---------|
| A | 16 consecutive UP presses, same channel | locate changing rolling field |
| B | 8 DOWN + 8 HALT, same channel | locate command/function bits |
| C | 4 UP on each channel available | locate channel/address bits |
| D | repeated packets during one long hold | learn whether retransmits reuse identical code |
| E | same sequence from a second remote, if available | separate unit identity from manufacturer constants |

For every capture record timestamp, remote model, selected channel, button,
short/long press, and exact raw RF bytes/bits.

### Highest-value free check before buying hardware

Open the physical master remote and photograph both PCB sides and every chip
marking. Finding an `HCS30x`, `HCS36x`, Microchip logo, or another encoder part
number would confirm or reject the KeeLoq hypothesis immediately. Do not alter
or erase the remote; only inspect it with the battery removed.

## Analysis

```bash
python3 research/analyze_captures.py
```

The analyzer enumerates the unknown two padding-bit alignments and common bit
orders, tests both 32+34 field layouts, and performs fixed/variable-bit analysis
as soon as more captures are appended to `corpus.json`.

## Decision gates

1. **Encoder IC identifies KeeLoq:** focus on frame mapping and key derivation;
   native Homey remains blocked until encryption is reproduced.
2. **Captures show 32 changing + 34 mostly fixed bits:** strong empirical KeeLoq
   confirmation; inspect static-field mapping and known KeeLoq conventions.
3. **No KeeLoq structure:** continue differential analysis; likely a custom
   whitening/substitution scheme in the USB stick.
4. **Cryptography cannot be reproduced:** use the official Becker USB stick or
   ESP32+CC1101 bridge as the RF endpoint; Homey app remains the UI/orchestrator.
