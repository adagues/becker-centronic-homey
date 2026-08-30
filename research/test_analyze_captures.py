#!/usr/bin/env python3

import json
import unittest
from pathlib import Path

from analyze_captures import (
    analysis_bits,
    differential_summary,
    hex_bits,
    selected_rf_bits,
    validate_capture,
    validate_corpus,
)


CORPUS_PATH = Path(__file__).with_name("corpus.json")


class CorpusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.corpus = json.loads(CORPUS_PATH.read_text())
        cls.local = [
            capture
            for capture in cls.corpus["captures"]
            if capture["campaign_id"] == "alexis-esp32-2026-08-30"
        ]

    def test_checked_in_corpus_is_valid(self):
        self.assertEqual(validate_corpus(self.corpus), [])

    def test_real_capture_campaign_contains_eight_distinct_presses(self):
        self.assertEqual(len(self.local), 8)
        self.assertEqual(
            [capture["capture_context"]["press_index"] for capture in self.local],
            list(range(1, 9)),
        )
        self.assertEqual(len({capture["rf_demodulated_hex"] for capture in self.local}), 8)

    def test_unknown_command_channel_and_logical_frame_are_not_invented(self):
        for capture in self.local:
            with self.subTest(capture=capture["id"]):
                self.assertIsNone(capture["logical_frame_hex"])
                self.assertIsNone(capture["capture_context"]["button"])
                self.assertIsNone(capture["capture_context"]["channel"])
                self.assertTrue(all(value is None for value in capture["known_fields"].values()))

    def test_only_two_local_entries_claim_complete_decoder_diagnostics(self):
        complete = [capture for capture in self.local if capture["rf_valid_bits"] is not None]
        payload_only = [capture for capture in self.local if capture["rf_valid_bits"] is None]
        self.assertEqual(len(complete), 2)
        self.assertEqual(len(payload_only), 6)
        self.assertTrue(all(capture["rf_valid_bits"] == 65 for capture in complete))
        self.assertTrue(
            all(
                capture["decode_quality"]["confidence"] == "documented-payload-only"
                for capture in payload_only
            )
        )

    def test_firmware_hex_padding_is_explicit_for_complete_frames(self):
        for capture in self.local[:2]:
            with self.subTest(capture=capture["id"]):
                selected = selected_rf_bits(capture)
                self.assertEqual(len(selected), 65)
                self.assertEqual(selected, hex_bits(capture["rf_demodulated_hex"])[:65])

    def test_provisional_projection_makes_all_local_entries_comparable(self):
        projected = [analysis_bits(capture) for capture in self.local]
        self.assertTrue(all(len(bits) == 64 for bits in projected))
        summary = differential_summary(self.local)
        self.assertEqual(summary["capture_count"], 8)
        self.assertEqual(summary["bit_length"], 64)
        self.assertEqual(len(summary["pairwise_hamming"]), 28)
        self.assertEqual(
            sorted(summary["variable_positions"] + summary["fixed_positions"]),
            list(range(64)),
        )
        self.assertTrue(all(pair["distance"] > 0 for pair in summary["pairwise_hamming"]))

    def test_invalid_projection_is_rejected(self):
        capture = dict(self.local[0])
        capture["analysis_projection"] = {
            "left_trim": 60,
            "bit_length": 64,
            "status": "provisional",
        }
        self.assertTrue(any("exceeds storage" in error for error in validate_capture(capture)))


if __name__ == "__main__":
    unittest.main()
