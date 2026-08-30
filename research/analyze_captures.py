#!/usr/bin/env python3
"""Validate and compare Becker Centronic RF captures without inventing fields.

The corpus contains both complete decoder records and historical payload-only
notes.  Differential analysis therefore uses an explicit ``analysis_projection``
for each comparable capture instead of silently assuming that every hex string
is a complete 65- or 66-bit frame.

Usage:
    python3 research/analyze_captures.py
    python3 research/analyze_captures.py path/to/corpus.json
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable


CONFIDENCE_LEVELS = {"reference", "repeat-verified", "documented-payload-only"}


def hex_bits(value: str) -> str:
    return f"{int(value, 16):0{len(value) * 4}b}"


def reverse_each_byte(bits: str) -> str:
    if len(bits) % 8:
        return bits
    return "".join(bits[i : i + 8][::-1] for i in range(0, len(bits), 8))


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


def selected_rf_bits(capture: dict) -> str:
    """Return a complete decoded RF frame only when its alignment is known."""
    valid_bits = capture.get("rf_valid_bits")
    left_trim = capture.get("rf_left_trim")
    if not isinstance(valid_bits, int) or not isinstance(left_trim, int):
        raise ValueError(f"{capture.get('id')}: RF frame length/alignment is unknown")
    return dict(valid_alignments(capture["rf_demodulated_hex"], valid_bits))[left_trim]


def analysis_bits(capture: dict) -> str:
    """Return the explicitly declared, possibly provisional comparison slice."""
    projection = capture.get("analysis_projection")
    if not isinstance(projection, dict):
        raise ValueError(f"{capture.get('id')}: no analysis_projection")
    raw = hex_bits(capture["rf_demodulated_hex"])
    left_trim = projection.get("left_trim")
    bit_length = projection.get("bit_length")
    if not isinstance(left_trim, int) or not isinstance(bit_length, int):
        raise ValueError(f"{capture.get('id')}: invalid analysis_projection")
    if left_trim < 0 or bit_length <= 0 or left_trim + bit_length > len(raw):
        raise ValueError(f"{capture.get('id')}: analysis_projection exceeds storage")
    return raw[left_trim : left_trim + bit_length]


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


def validate_capture(capture: dict) -> list[str]:
    errors: list[str] = []
    capture_id = capture.get("id", "<missing-id>")
    for field in ("id", "campaign_id", "source", "rf_demodulated_hex", "decode_quality"):
        if field not in capture:
            errors.append(f"{capture_id}: missing {field}")

    raw_hex = capture.get("rf_demodulated_hex")
    if not isinstance(raw_hex, str) or not raw_hex:
        errors.append(f"{capture_id}: rf_demodulated_hex must be a non-empty string")
        return errors
    try:
        raw = hex_bits(raw_hex)
    except ValueError:
        errors.append(f"{capture_id}: rf_demodulated_hex is not hexadecimal")
        return errors

    valid_bits = capture.get("rf_valid_bits")
    left_trim = capture.get("rf_left_trim")
    if valid_bits is not None:
        if not isinstance(valid_bits, int) or valid_bits <= 0 or valid_bits > len(raw):
            errors.append(f"{capture_id}: invalid rf_valid_bits")
        elif left_trim is not None and (
            not isinstance(left_trim, int)
            or left_trim < 0
            or left_trim + valid_bits > len(raw)
        ):
            errors.append(f"{capture_id}: invalid rf_left_trim")
    elif left_trim is not None:
        errors.append(f"{capture_id}: rf_left_trim requires rf_valid_bits")

    projection = capture.get("analysis_projection")
    if projection is not None:
        try:
            analysis_bits(capture)
        except ValueError as exc:
            errors.append(str(exc))

    logical = capture.get("logical_frame_hex")
    if logical is not None:
        try:
            parse_logical(logical)
        except (TypeError, ValueError) as exc:
            errors.append(f"{capture_id}: {exc}")

    quality = capture.get("decode_quality")
    if isinstance(quality, dict):
        confidence = quality.get("confidence")
        if confidence not in CONFIDENCE_LEVELS:
            errors.append(f"{capture_id}: invalid confidence {confidence!r}")
    return errors


def validate_corpus(corpus: dict) -> list[str]:
    errors: list[str] = []
    if corpus.get("schema") != 2:
        errors.append("corpus: expected schema 2")
    campaigns = corpus.get("campaigns")
    captures = corpus.get("captures")
    if not isinstance(campaigns, dict):
        errors.append("corpus: campaigns must be an object")
        campaigns = {}
    if not isinstance(captures, list):
        return errors + ["corpus: captures must be an array"]

    seen_ids: set[str] = set()
    for capture in captures:
        if not isinstance(capture, dict):
            errors.append("corpus: capture entries must be objects")
            continue
        capture_id = capture.get("id")
        if capture_id in seen_ids:
            errors.append(f"corpus: duplicate capture id {capture_id}")
        if isinstance(capture_id, str):
            seen_ids.add(capture_id)
        if capture.get("campaign_id") not in campaigns:
            errors.append(f"{capture_id}: unknown campaign_id")
        errors.extend(validate_capture(capture))
    return errors


def print_single_capture(capture: dict) -> None:
    print(f"\n[{capture['id']}]")
    logical_hex = capture.get("logical_frame_hex")
    print("logical:", parse_logical(logical_hex) if logical_hex else "unknown")
    valid_bits = capture.get("rf_valid_bits")
    valid_label = str(valid_bits) if valid_bits is not None else "unknown"
    print(
        f"RF: {capture['rf_demodulated_hex']} / "
        f"{valid_label} valid bits / clock≈{capture.get('clock_us')} µs"
    )
    projection = capture.get("analysis_projection")
    if projection:
        print(
            f"  comparison projection: {projection['bit_length']} bits "
            f"({projection['status']}) -> {analysis_bits(capture)}"
        )

    if not logical_hex or not isinstance(valid_bits, int):
        return

    logical = parse_logical(logical_hex)
    unit20 = f"{int(str(logical['unit']), 16):020b}"
    command8 = f"{int(str(logical['command']), 16):08b}"
    counter16 = f"{int(logical['counter']):016b}"
    storage_bits = len(hex_bits(capture["rf_demodulated_hex"]))
    for trim, aligned in valid_alignments(capture["rf_demodulated_hex"], valid_bits):
        right_trim = storage_bits - valid_bits - trim
        print(f"  alignment left-trim={trim}, right-trim={right_trim}: {aligned}")
        for order, bits in unique_variants(aligned).items():
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


def differential_summary(captures: list[dict]) -> dict:
    if len(captures) < 2:
        raise ValueError("differential analysis needs at least two captures")
    normalized = [analysis_bits(capture) for capture in captures]
    lengths = {len(bits) for bits in normalized}
    if len(lengths) != 1:
        raise ValueError("analysis projections must have equal lengths")

    bit_length = lengths.pop()
    variable = [
        position
        for position in range(bit_length)
        if len({bits[position] for bits in normalized}) > 1
    ]
    fixed = [position for position in range(bit_length) if position not in variable]
    pairwise = []
    for left_index, left in enumerate(captures):
        for right_index in range(left_index + 1, len(captures)):
            pairwise.append(
                {
                    "left": left["id"],
                    "right": captures[right_index]["id"],
                    "distance": hamming(normalized[left_index], normalized[right_index]),
                }
            )
    return {
        "capture_count": len(captures),
        "bit_length": bit_length,
        "variable_positions": variable,
        "fixed_positions": fixed,
        "pairwise_hamming": pairwise,
    }


def print_differential(captures: list[dict]) -> None:
    groups: dict[str, list[dict]] = defaultdict(list)
    for capture in captures:
        group = capture.get("analysis_group")
        if group:
            groups[group].append(capture)
    if not groups:
        print("\nDifferential analysis: no explicit comparison groups.")
        return

    for group_name, group_captures in sorted(groups.items()):
        print(f"\nDifferential analysis group: {group_name}")
        if len(group_captures) < 2:
            print("  need at least two captures")
            continue
        summary = differential_summary(group_captures)
        base_id = group_captures[0]["id"]
        for pair in summary["pairwise_hamming"]:
            if pair["left"] == base_id:
                print(
                    f"  {pair['right']}: {pair['distance']} / "
                    f"{summary['bit_length']} bits changed from {base_id}"
                )
        print("  variable bit positions:", summary["variable_positions"])
        print("  fixed bit positions:", summary["fixed_positions"])
        print("  WARNING: this group uses provisional 64-bit projections, not verified complete frames.")


def main() -> int:
    corpus_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("corpus.json")
    corpus = json.loads(corpus_path.read_text())
    errors = validate_corpus(corpus)
    if errors:
        print("Corpus validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 2

    captures = corpus["captures"]
    print(f"Loaded {len(captures)} capture(s) from {corpus_path}")
    print("Hypothesis under test: KeeLoq-like framing remains unconfirmed.")
    for capture in captures:
        print_single_capture(capture)
    print_differential(captures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
