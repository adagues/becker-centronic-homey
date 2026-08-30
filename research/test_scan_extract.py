#!/usr/bin/env python3

import unittest

from keeloq import encrypt
from scan_extract import Segment, parse_intel_hex, scan_segments


class ExtractScannerTests(unittest.TestCase):
    KEY = 0x0123456789ABCDEF
    PAIRS = [
        (0x4ABC0001, encrypt(0x4ABC0001, KEY)),
        (0x4ABC0002, encrypt(0x4ABC0002, KEY)),
    ]

    def test_finds_big_endian_key_in_binary(self):
        image = b"HEADER" + self.KEY.to_bytes(8, "big") + b"TRAILER"
        hits = scan_segments([Segment(0x100, image)], self.PAIRS)
        self.assertTrue(any(hit.key == self.KEY and hit.address == 0x106 for hit in hits))

    def test_finds_little_endian_key_in_binary(self):
        image = b"ABC" + self.KEY.to_bytes(8, "little") + b"XYZ"
        hits = scan_segments([Segment(0, image)], self.PAIRS)
        self.assertTrue(any(hit.key == self.KEY and hit.address == 3 for hit in hits))

    def test_wrong_pairs_do_not_match(self):
        image = self.KEY.to_bytes(8, "big")
        wrong = [(self.PAIRS[0][0], self.PAIRS[0][1] ^ 1)]
        self.assertEqual(scan_segments([Segment(0, image)], wrong), [])

    def test_parses_intel_hex_segment(self):
        # Eight data bytes at address 0x0010, then EOF.
        payload = self.KEY.to_bytes(8, "big")
        record = bytes([8, 0, 0x10, 0]) + payload
        checksum = (-sum(record)) & 0xFF
        text = ":" + (record + bytes([checksum])).hex().upper() + "\n:00000001FF\n"
        segments = parse_intel_hex(text)
        self.assertEqual(segments, [Segment(0x10, payload)])
        hits = scan_segments(segments, self.PAIRS)
        self.assertTrue(any(hit.key == self.KEY and hit.address == 0x10 for hit in hits))


if __name__ == "__main__":
    unittest.main()
