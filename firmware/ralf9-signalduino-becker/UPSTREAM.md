# Upstream record

Vendored from:

- Repository: https://github.com/Ralf9/SIGNALDuino
- Branch: dev-r422_cc1101
- Commit: 2fc2c8a718cd232ca8ce24c426784e720fcc7167
- Retrieved: 2026-08-29

Local adaptations:

1. Added `SIGNALESP32_BECKER`: VSPI 18/19/23, CSN 5, GDO2 4.
2. Added `RALF9_BECKER_SERIAL_ONLY`: USB serial output, no Wi-Fi startup.
3. Added `RALF9_BECKER_PROFILE`: compiled Becker register profile and TEST
   registers, independent of EEPROM contents.
4. Added deterministic startup state: radio B, bank 0, `ccmode=0`, RX and
   Manchester enabled; EEPROM commits only when correction is needed.
5. Added a pinned PlatformIO environment and wiring/usage documentation.

No upstream history is rewritten. The vendored code and local derivative remain
under GPL-3.0; see `LICENSE`.
