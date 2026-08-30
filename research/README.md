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

## Blind KeeLoq implementation

`keeloq.py` now provides:

- the standard 528-round KeeLoq encrypt/decrypt primitive;
- a conventional candidate plaintext layout (`function | discrimination |
  counter`);
- eight common 66-bit field/bit-order variants (`hop32 + fixed34` and the
  reverse);
- candidate normal-learning key derivations for testing once a manufacturer
  key candidate is available.

Run its tests with:

```bash
python3 -m unittest discover -s research -p 'test_*.py'
```

This is intentionally a parameterized research implementation. It does not
contain a Becker device/manufacturer key, and therefore cannot yet produce a
rolling code that a Becker receiver will accept. Captures and/or readout of the
owner's PIC16F636 are still required to determine Becker's exact field layout,
key and key-derivation convention.

## Corpus schema v2 and evidence levels

`corpus.json` now separates campaign-wide facts from individual captures. Every
capture records its provenance, context, decoder quality and unknown fields.
Unknown values stay `null`; they must never be inferred from payload appearance.

Three evidence levels are accepted:

- `reference`: a published capture with independently documented parameters;
- `repeat-verified`: a full local decoder line whose payload repeated during the
  same button press;
- `documented-payload-only`: a historical payload without its raw pulse train or
  exact decoder bit count.

The eight local payloads are retained, but only the first two contain complete
65-bit diagnostics. For cross-press exploration all eight declare an explicit,
**provisional** 64-bit projection. This projection is suitable for locating
candidate stable/variable positions; it is not evidence that the six
payload-only records are complete 64-bit RF frames.

The analyzer validates the schema before printing results:

```bash
python3 research/analyze_captures.py
python3 -m unittest discover -s research -p 'test_*.py'
```

### Next capture campaign

Create one corpus entry for every decoded repeat, not just one line per press.
Retain the complete serial `MC ... D=...` line or the `/frames` JSON response.
For each press record:

| Field | Requirement |
|-------|-------------|
| timestamp | UTC ISO-8601 |
| remote | stable anonymous identifier and exact model when known |
| button/channel | explicit value; use `null` when genuinely unknown |
| press | short/long plus monotonically increasing press index |
| repeat | repeat index within the press |
| decoder | bit count, clock, pulse count, half-bit count, phase and glitches |
| RF | full demodulated hex and signed raw pulse durations |
| receiver | firmware commit, centre frequency, modulation and RSSI |

Capture at minimum 16 consecutive presses of one button/channel, 8 presses each
of two other commands, every available channel, and one long hold. A capture is
eligible for framing analysis only when all repeats inside a press normalize to
the same bitstream and no half-bit loss is reported. Keep questionable records
in the corpus with a caveat; do not silently discard or repair them.
