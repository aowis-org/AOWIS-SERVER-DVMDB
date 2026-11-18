#!/usr/bin/env python3
"""Regression coverage for the offline backup verifier's MODEL journal envelope."""

import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from offline_backup import valid_model_edit_frame


# UUIDv7 shape, matching the event identifiers written by ProjectService.
EVENT_ID = bytes.fromhex("019a2bc345677def8123456789abcdef")


def make_frame(version=2):
    payload = EVENT_ID + b"hydraulic.junction.move"
    return (b"AOWE" + bytes([version]) + len(payload).to_bytes(4, "big") +
            payload + hashlib.sha256(payload).digest())


class OfflineBackupFrameTest(unittest.TestCase):
    def test_current_format_is_accepted(self):
        self.assertTrue(valid_model_edit_frame(make_frame(), EVENT_ID))

    def test_legacy_format_is_rejected_even_with_valid_digest(self):
        self.assertFalse(valid_model_edit_frame(make_frame(version=1), EVENT_ID))

    def test_corrupt_digest_and_wrong_event_id_are_rejected(self):
        frame = make_frame()
        self.assertFalse(valid_model_edit_frame(frame[:-1] + bytes([frame[-1] ^ 1]), EVENT_ID))
        self.assertFalse(valid_model_edit_frame(frame, bytes(16)))

    def test_corrupt_length_and_truncated_frame_are_rejected(self):
        frame = make_frame()
        self.assertFalse(valid_model_edit_frame(frame[:5] + bytes(4) + frame[9:], EVENT_ID))
        self.assertFalse(valid_model_edit_frame(frame[:-1], EVENT_ID))
        self.assertFalse(valid_model_edit_frame(b"AOWE\x02", EVENT_ID))


if __name__ == "__main__":
    unittest.main()
