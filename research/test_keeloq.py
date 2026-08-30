#!/usr/bin/env python3

import unittest

from keeloq import (
    StandardFields,
    build_66bit_candidates,
    decrypt,
    derive_normal_learning_candidates,
    encrypt,
)


class KeeLoqTests(unittest.TestCase):
    def test_round_trip_reference_samples(self):
        samples = (
            (0x00000000, 0x0000000000000000),
            (0x12345678, 0x0123456789ABCDEF),
            (0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF),
            (0xF741E2DB, 0x5CEC6701B79FD949),
        )
        for plaintext, key in samples:
            with self.subTest(plaintext=plaintext, key=key):
                self.assertEqual(decrypt(encrypt(plaintext, key), key), plaintext)

    def test_cipher_is_not_identity(self):
        self.assertNotEqual(encrypt(0x12345678, 0x0123456789ABCDEF), 0x12345678)

    def test_standard_candidate_shape(self):
        fields = StandardFields(
            counter=1,
            discrimination=0xABC,
            function=4,
            serial=0x1234567,
        )
        candidates = build_66bit_candidates(fields, 0x0123456789ABCDEF)
        self.assertEqual(len(candidates), 8)
        self.assertTrue(all(len(value) == 66 for value in candidates.values()))
        self.assertTrue(all(set(value) <= {"0", "1"} for value in candidates.values()))

    def test_plaintext_candidate_layout(self):
        fields = StandardFields(
            counter=0x1234,
            discrimination=0xABC,
            function=4,
            serial=0,
        )
        self.assertEqual(fields.plaintext(), 0x4ABC1234)

    def test_kdf_candidates_are_64_bit(self):
        candidates = derive_normal_learning_candidates(
            serial=0x01234567,
            manufacturer_key=0x0123456789ABCDEF,
        )
        self.assertGreater(len(candidates), 0)
        self.assertTrue(all(0 <= key <= 0xFFFFFFFFFFFFFFFF for key in candidates.values()))


if __name__ == "__main__":
    unittest.main()
