#!/usr/bin/env python3
"""Analyze Becker Centronic logical-frame / RF-code pairs.

This tool deliberately makes no assumption about the unpublished transformation.
It enumerates padding alignment and bit-order variants, compares captures, and
scores whether a 66-bit payload has the structural shape of classic KeeLoq:
32 hopping bits + 34 fixed/status bits (or the reverse ordering).

Usage:
    python3 research/analyze_captures.py
    python3 research/analyze_captures.py path/to/corpus.json
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable


def hex_bits(value: str) -> str:
    return f"{int(value, 16):0{len(value) * 4}b}"


def reverse_each_byte(bits: str) -> str:
    if len(bits) % 8:
        return bits
    return "".join(bits[i : i + 8][::-1] for i in range(0, len(bits), 8))


def variants(bits: str) -> dict[str, str]:
    values = {
        "wire": bits,
        "wire-reversed": bits[::-1],
        "byte-order-reversed": "".join(
            bits[i : i + 8] for i in range(0, len(bits), 8)
        )[::-1]
        if len(bits) % 8 == 0
        else bits[::-1],
        "bits-in-each-byte-reversed": reverse_each_byte(bits),
    }
    return dict.fromkeys(values.items())  # type: ignore[return-value]


def unique_variants(bits: str) -> dict[str, str]:
    result: dict[str, str] = {}
    seen: set[str] = set()
    for name, value in {
        "wire": bits,
        "wire-reversed": bits[::-1],
        "bits-in-each-byte-reversed": reverse_each_byte(bits),
    }.items():
        if value not in seen:
            result[name] = value
            seen.add(value)
    return result


def valid_alignments(raw_hex: str, valid_bits: int) -> Iterable[tuple[int, str]]:
    raw = hex_bits(raw_hex)
    padding = len(raw) - valid_bits
    if padding < 0:
        raise ValueError(f"valid_bits={valid_bits} exceeds {len(raw)} storage bits")
    for left_trim in range(padding + 1):
        yield left_trim, raw[left_trim : left_trim + valid_bits]


def hamming(a: str, b: str) -> int:
    if len(a) != len(b):
        raise ValueError("Hamming inputs must have equal length")
    return sum(x != y for x, y in zip(a, b))


def parse_logical(frame: str) -> dict[str, str | int]:
    if len(frame) != 42:
        raise ValueError(f"logical frame must be 42 hex chars, got {len(frame)}")
    return {
        "prefix": frame[0:14],
        "counter": int(frame[14:18], 16),
        "suffix": frame[18:24],
        "unit": frame[24:29],
        "marker": frame[29:32],
        "remote": frame[32:34],
        "channel": int(frame[34:36], 16),
        "reserved": frame[36:38],
        "command": frame[38:40],
        "checksum": frame[40:42],
    }


def print_single_capture(capture: dict) -> None:
    logical = parse_logical(capture["logical_frame_hex"])
    print(f"\n[{capture['id']}]")
    print("logical:", logical)
    print(
        f"RF: {capture['rf_demodulated_hex']} / "
        f"{capture['rf_valid_bits']} valid bits / clock≈{capture.get('clock_us')} µs"
    )
    unit20 = f"{int(str(logical['unit']), 16):020b}"
    command8 = f"{int(str(logical['command']), 16):08b}"
    counter16 = f"{int(logical['counter']):016b}"
    for trim, aligned in valid_alignments(
        capture["rf_demodulated_hex"], capture["rf_valid_bits"]
    ):
        print(f"  alignment left-trim={trim}, right-trim={2-trim}: {aligned}")
        for order, bits in unique_variants(aligned).items():
            # KeeLoq/HCS301-like payloads are 66 bits: 32 encrypted hopping bits
            # plus 34 fixed/function/status bits. Test both possible field orders.
            for layout, fixed in (("hop32|fixed34", bits[32:]), ("fixed34|hop32", bits[:34])):
                hits = []
                for label, needle in (
                    ("unit20", unit20),
                    ("unit20-reversed", unit20[::-1]),
                    ("command8", command8),
                    ("command8-reversed", command8[::-1]),
                    ("counter16", counter16),
                    ("counter16-reversed", counter16[::-1]),
                ):
                    pos = fixed.find(needle)
                    if pos >= 0:
                        hits.append(f"{label}@{pos}")
                suffix = ", ".join(hits) if hits else "no direct known-field hit"
                print(f"    {order:28} {layout:14} -> {suffix}")


def print_differential(captures: list[dict]) -> None:
    if len(captures) < 2:
        print("\nDifferential analysis: need at least 2 captures; corpus currently has 1.")
        return
    # Use explicitly selected alignment when present, otherwise every entry must
    # use alignment 0. Captures should be normalized after first inspection.
    normalized = []
    for c in captures:
        trim = c.get("rf_left_trim", 0)
        aligned = dict(valid_alignments(c["rf_demodulated_hex"], c["rf_valid_bits"]))[trim]
        normalized.append(aligned)
    base = normalized[0]
    print("\nDifferential analysis (relative to first capture):")
    for c, bits in zip(captures[1:], normalized[1:]):
        print(f"  {c['id']}: {hamming(base, bits)} / {len(base)} bits changed")
    variability = Counter(i for i in range(len(base)) if len({x[i] for x in normalized}) > 1)
    changed = sorted(variability)
    print("  variable bit positions:", changed)
    fixed = [i for i in range(len(base)) if i not in changed]
    print("  fixed bit positions:", fixed)


def main() -> None:
    corpus_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("corpus.json")
    corpus = json.loads(corpus_path.read_text())
    captures = corpus["captures"]
    print(f"Loaded {len(captures)} capture(s) from {corpus_path}")
    print("Hypothesis under test: 66-bit KeeLoq-like frame (32 hopping + 34 fixed/status).")
    for capture in captures:
        print_single_capture(capture)
    print_differential(captures)


if __name__ == "__main__":
    main()
