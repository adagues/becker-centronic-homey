# Becker Centronic for Homey Pro

Control **Becker Centronic** roller shutters (classic, pre-CentronicPlus) natively
with the **868 MHz radio built into Homey Pro (Early 2019)** — no USB stick, no
bridge hardware.

## How it works

The app acts as one virtual Becker multi-channel remote:

- One **unit id** (20-bit sender identity) is generated per install.
- Each Homey shutter device occupies one **channel (1–7)** of that unit.
- A **rolling counter** (16-bit, +1 per command, receiver accepts a window of
  +1..+49) is persisted in app settings and shared across devices via a
  serialized transmit queue.

Protocol knowledge is ported from
[ole1986/centronic-py](https://github.com/ole1986/centronic-py/blob/master/TECHNICAL.md)
(reverse-engineered Becker Centronic USB stick protocol).

### Frame layout (21 bytes)

```
0000000002010B IIII 000000 UUUUU 021 RR CC 00 MM KK
|-- prefix --| incr suffix unit      rem ch    cmd checksum
```

- `checksum = (0x03 - sum(bytes)) & 0xFF`
- Commands: `HALT 0x10`, `UP 0x20`, `DOWN 0x40`, intermediates `0x24/0x44`,
  pairing `0x80–0x83`, clear-position `0x90–0x93`.

## Pairing a shutter

1. Add the device in Homey (pick a free channel).
2. Put the shutter motor into programming mode with your existing **master
   remote** (hold its programming button until the motor confirms).
3. Run the Flow action **"Pair (train) with shutter"** within the pairing
   window. The motor confirms with a clack/movement.

## Counter re-sync

If the receiver ever rejects commands (counter drift), run the Flow action
**"Re-sync rolling counter"** — Becker receivers accept a new counter value
when the same command arrives three times in a row.

## ⚠️ Status: RF signal calibration required

The command layer (frames, checksum, counter, pairing sequences) is complete
and unit-tested. The **on-air signal definition** in `app.json` (`signals.868.becker`)
is derived from FHEM/SIGNALduino captures — 868.283 MHz, 2-FSK,
Manchester-coded, half-bit ≈ 417 µs, 65-bit frames — and the mapping from the
21-byte stick frame to the 65-bit on-air frame **must be verified against a
capture of a real remote** before the first successful transmission.
If you own an RTL-SDR or a nanoCUL, a capture of your remote's UP button is
enough to finish the calibration.

## Development

```bash
npm test                  # protocol unit tests (plain node)
homey app validate        # validate the manifest
homey app run             # run on your Homey Pro
```

## Credits

- Protocol reverse engineering: [ole1986/centronic-py](https://github.com/ole1986/centronic-py)
- Related work: [nicolasberthel/pybecker](https://github.com/nicolasberthel/pybecker)
