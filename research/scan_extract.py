#!/usr/bin/env python3
"""Scan an authorized PIC firmware/EEPROM extract for a KeeLoq key.

This is intentionally NOT a 2^64 brute-forcer. It tests every 8-byte window
already present in a binary or Intel HEX extract under common byte/word orders,
then validates candidates against known plaintext:ciphertext pairs.

Examples:
  python3 research/scan_extract.py remote.hex \
    --pair 0x4ABC0001:0x12345678 \
    --pair 0x4ABC0002:0x9ABCDEF0

Use two or more independent pairs. One 32-bit pair leaves many theoretical
64-bit keys and is insufficient to prove uniqueness over the whole key space.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from keeloq import encrypt


@dataclass(frozen=True)
class Segment:
    address: int
    data: bytes


@dataclass(frozen=True)
class Hit:
    address: int
    layout: str
    key: int
    raw: bytes


def parse_intel_hex(text: str) -> list[Segment]:
    """Parse data records from an Intel HEX file into contiguous segments."""
    memory: dict[int, int] = {}
    upper = 0
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"line {line_number}: not Intel HEX")
        record = bytes.fromhex(line[1:])
        if len(record) < 5:
            raise ValueError(f"line {line_number}: short record")
        length = record[0]
        if len(record) != length + 5:
            raise ValueError(f"line {line_number}: invalid byte count")
        if sum(record) & 0xFF:
            raise ValueError(f"line {line_number}: checksum mismatch")
        offset = (record[1] << 8) | record[2]
        record_type = record[3]
        payload = record[4 : 4 + length]
        if record_type == 0x00:
            base = upper + offset
            for index, value in enumerate(payload):
                memory[base + index] = value
        elif record_type == 0x01:
            break
        elif record_type == 0x02:
            if length != 2:
                raise ValueError(f"line {line_number}: bad extended segment address")
            upper = int.from_bytes(payload, "big") << 4
        elif record_type == 0x04:
            if length != 2:
                raise ValueError(f"line {line_number}: bad extended linear address")
            upper = int.from_bytes(payload, "big") << 16
        # Other record types contain metadata/start addresses, not image bytes.

    if not memory:
        return []
    segments: list[Segment] = []
    addresses = sorted(memory)
    start = addresses[0]
    previous = start
    data = bytearray([memory[start]])
    for address in addresses[1:]:
        if address == previous + 1:
            data.append(memory[address])
        else:
            segments.append(Segment(start, bytes(data)))
            start = address
            data = bytearray([memory[address]])
        previous = address
    segments.append(Segment(start, bytes(data)))
    return segments


def load_segments(path: Path) -> list[Segment]:
    raw = path.read_bytes()
    stripped = raw.lstrip()
    if stripped.startswith(b":"):
        return parse_intel_hex(raw.decode("ascii"))
    return [Segment(0, raw)]


def key_layouts(raw: bytes) -> dict[str, int]:
    """Interpret one 8-byte window using common PIC/storage orders."""
    if len(raw) != 8:
        raise ValueError("key window must be exactly 8 bytes")
    forms = {
        "big-endian": raw,
        "little-endian": raw[::-1],
        "swap-32bit-words": raw[4:8] + raw[0:4],
        "reverse-each-32bit-word": raw[0:4][::-1] + raw[4:8][::-1],
    }
    result: dict[str, int] = {}
    seen: set[int] = set()
    for name, form in forms.items():
        key = int.from_bytes(form, "big")
        if key not in seen:
            result[name] = key
            seen.add(key)
    return result


def scan_segments(
    segments: Iterable[Segment],
    pairs: list[tuple[int, int]],
    *,
    stride: int = 1,
) -> list[Hit]:
    if not pairs:
        raise ValueError("at least one plaintext:ciphertext pair is required")
    hits: list[Hit] = []
    seen_hits: set[tuple[int, int]] = set()
    for segment in segments:
        for offset in range(0, max(0, len(segment.data) - 7), stride):
            raw = segment.data[offset : offset + 8]
            # Erased/unprogrammed flash and all-zero padding are not useful.
            if raw in (b"\x00" * 8, b"\xFF" * 8):
                continue
            for layout, key in key_layouts(raw).items():
                if all(encrypt(plain, key) == cipher for plain, cipher in pairs):
                    identity = (segment.address + offset, key)
                    if identity not in seen_hits:
                        hits.append(Hit(segment.address + offset, layout, key, raw))
                        seen_hits.add(identity)
    return hits


def parse_pair(value: str) -> tuple[int, int]:
    try:
        plain_text, cipher_text = value.split(":", 1)
        return int(plain_text, 0) & 0xFFFFFFFF, int(cipher_text, 0) & 0xFFFFFFFF
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError(
            "pair must be PLAINTEXT:CIPHERTEXT (for example 0x1234:0x5678)"
        ) from error


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extract", type=Path, help="PIC binary or Intel HEX dump")
    parser.add_argument(
        "--pair",
        action="append",
        type=parse_pair,
        required=True,
        help="known 32-bit plaintext:ciphertext pair; repeat at least twice",
    )
    parser.add_argument("--stride", type=int, default=1, choices=range(1, 9))
    args = parser.parse_args()

    if len(args.pair) < 2:
        print("WARNING: one pair can identify a candidate in a small dump but cannot prove uniqueness.")
    segments = load_segments(args.extract)
    print(f"Loaded {len(segments)} segment(s), {sum(len(s.data) for s in segments)} data bytes")
    hits = scan_segments(segments, args.pair, stride=args.stride)
    if not hits:
        print("No contiguous 64-bit key matched the supplied pair(s).")
        print("The key may be protected, transformed, split, or the frame/plaintext mapping may be wrong.")
        raise SystemExit(1)
    for hit in hits:
        print(
            f"MATCH address=0x{hit.address:X} layout={hit.layout} "
            f"key=0x{hit.key:016X} raw={hit.raw.hex().upper()}"
        )


if __name__ == "__main__":
    main()
