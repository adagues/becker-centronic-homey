#!/usr/bin/env python3
"""Standard KeeLoq primitive and blind 66-bit frame candidate generator.

Research use for the owner's Becker Centronic remote. This does NOT contain a
Becker key and cannot create accepted rolling codes without one.

Cipher ported from the public 528-round reference implementation:
https://github.com/chezerian/keeloq/blob/master/keeloq.c
"""

from __future__ import annotations

from dataclasses import dataclass

NLF = 0x3A5C742E
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


def bit(value: int, index: int) -> int:
    return (value >> index) & 1


def g5(value: int, a: int, b: int, c: int, d: int, e: int) -> int:
    return (
        bit(value, a)
        | (bit(value, b) << 1)
        | (bit(value, c) << 2)
        | (bit(value, d) << 3)
        | (bit(value, e) << 4)
    )


def encrypt(plaintext: int, key: int) -> int:
    """Encrypt one 32-bit block using a 64-bit key and 528 KeeLoq rounds."""
    x = plaintext & MASK32
    key &= MASK64
    for round_index in range(528):
        feedback = (
            bit(x, 0)
            ^ bit(x, 16)
            ^ bit(key, round_index & 63)
            ^ bit(NLF, g5(x, 1, 9, 20, 26, 31))
        )
        x = ((x >> 1) | (feedback << 31)) & MASK32
    return x


def decrypt(ciphertext: int, key: int) -> int:
    """Decrypt one 32-bit block using a 64-bit key and 528 KeeLoq rounds."""
    x = ciphertext & MASK32
    key &= MASK64
    for round_index in range(528):
        feedback = (
            bit(x, 31)
            ^ bit(x, 15)
            ^ bit(key, (15 - round_index) & 63)
            ^ bit(NLF, g5(x, 0, 8, 19, 25, 30))
        )
        x = ((x << 1) & MASK32) ^ feedback
    return x & MASK32


def reverse_bits(value: int, width: int) -> int:
    result = 0
    for index in range(width):
        result = (result << 1) | bit(value, index)
    return result


def bits(value: int, width: int, *, lsb_first: bool = False) -> str:
    result = f"{value & ((1 << width) - 1):0{width}b}"
    return result[::-1] if lsb_first else result


@dataclass(frozen=True)
class StandardFields:
    """Common HCS200/HCS301-style fields; Becker's mapping is still unknown."""

    counter: int  # 16-bit synchronization counter
    discrimination: int  # common implementation uses 12 bits
    function: int  # 4 button/function bits
    serial: int  # commonly 28 transmitted serial bits
    battery_low: int = 0
    repeat: int = 0

    def plaintext(self) -> int:
        """Conventional candidate: function[31:28], discr[27:16], counter[15:0]."""
        return (
            ((self.function & 0xF) << 28)
            | ((self.discrimination & 0xFFF) << 16)
            | (self.counter & 0xFFFF)
        )

    def fixed34(self) -> int:
        """Candidate fixed section: serial28, function4, battery-low, repeat."""
        return (
            ((self.serial & 0x0FFFFFFF) << 6)
            | ((self.function & 0xF) << 2)
            | ((self.battery_low & 1) << 1)
            | (self.repeat & 1)
        )


def build_66bit_candidates(fields: StandardFields, key: int) -> dict[str, str]:
    """Enumerate common field order and per-field bit-order candidates.

    The returned values are Manchester-decoded data bits, not RF pulses.
    """
    hopping = encrypt(fields.plaintext(), key)
    fixed = fields.fixed34()
    variants: dict[str, str] = {}
    for hop_lsb in (False, True):
        for fixed_lsb in (False, True):
            h = bits(hopping, 32, lsb_first=hop_lsb)
            f = bits(fixed, 34, lsb_first=fixed_lsb)
            suffix = f"hop-{'lsb' if hop_lsb else 'msb'}_fixed-{'lsb' if fixed_lsb else 'msb'}"
            variants[f"hop32|fixed34_{suffix}"] = h + f
            variants[f"fixed34|hop32_{suffix}"] = f + h
    return variants


def derive_normal_learning_candidates(serial: int, manufacturer_key: int) -> dict[str, int]:
    """Return common normal-learning KDF candidates without claiming Becker's KDF.

    Vendors vary the marker bits, order and encryption direction. These are
    deliberately labelled candidates for corpus testing, not authoritative
    Becker behavior.
    """
    serial28 = serial & 0x0FFFFFFF
    inputs = {
        "serial": serial28,
        "serial|0x20000000": serial28 | 0x20000000,
        "serial|0x60000000": serial28 | 0x60000000,
    }
    result: dict[str, int] = {}
    for direction, operation in (("enc", encrypt), ("dec", decrypt)):
        words = {name: operation(value, manufacturer_key) for name, value in inputs.items()}
        for high_name, high in words.items():
            for low_name, low in words.items():
                if high_name == low_name:
                    continue
                result[f"{direction}:{high_name}|{low_name}"] = ((high << 32) | low) & MASK64
    return result


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate standard KeeLoq 66-bit candidates (research only)."
    )
    parser.add_argument("--key", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--serial", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--counter", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--function", required=True, type=lambda value: int(value, 0))
    parser.add_argument(
        "--discrimination",
        type=lambda value: int(value, 0),
        help="12-bit value; defaults to serial & 0xfff",
    )
    args = parser.parse_args()
    fields = StandardFields(
        counter=args.counter,
        discrimination=(
            args.discrimination
            if args.discrimination is not None
            else args.serial & 0xFFF
        ),
        function=args.function,
        serial=args.serial,
    )
    print(f"plaintext=0x{fields.plaintext():08X}")
    print(f"hopping=0x{encrypt(fields.plaintext(), args.key):08X}")
    for name, candidate in build_66bit_candidates(fields, args.key).items():
        print(f"{name}: {candidate}")


if __name__ == "__main__":
    main()
