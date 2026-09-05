# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from neognss_observatory.sbas_extract import continuous_groups, extract_group


class Progress:
    def update(self, size):
        pass


class SbasBatchTest(unittest.TestCase):
    def test_midnight_is_not_a_reset_but_gap_is(self):
        segments = [
            dict(start_utc=86390, end_utc=86399),
            dict(start_utc=86400, end_utc=86403),
            dict(start_utc=86405, end_utc=86406),
        ]
        self.assertEqual([len(g) for g in continuous_groups(segments)], [2, 1])

    def test_overlap_is_rejected(self):
        with self.assertRaises(ValueError):
            continuous_groups([dict(start_utc=1, end_utc=3), dict(start_utc=3, end_utc=5)])

    def test_partial_frame_survives_continuous_file_boundary(self):
        worker = Path("build/libcppubx2/cppubx2_subframes").resolve()
        if not worker.exists():
            self.skipTest("Build cppubx2_subframes first")
        data = bytes.fromhex("5348a30003ffc001ffc000fff0003ff0000ffc0003fc0000180000002b2e7000")
        payload = bytes([1, 137, 0, 0, 8, 18, 2, 0]) + struct.pack("<8I", *struct.unpack(">8I", data))
        body = bytes([2, 0x13, len(payload), 0]) + payload
        a = b = 0
        for byte in body:
            a = (a + byte) & 255
            b = (b + a) & 255
        frame = b"\xb5\x62" + body + bytes([a, b])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            group = []
            for i, chunk in enumerate((frame[:19], frame[19:] + frame)):
                path = root / f"{i}.ubx"
                path.write_bytes(chunk)
                group.append(
                    dict(
                        name=path.name,
                        size=len(chunk),
                        sha256=hashlib.sha256(chunk).hexdigest(),
                        mtime_ns=path.stat().st_mtime_ns,
                        start_utc=86399 + i,
                        end_utc=86399 + i,
                    )
                )
            summary = extract_group(root, group, root / "out", worker, Progress())
            self.assertEqual(summary["sbas_status"], {"decoded": 2})
            self.assertEqual(summary["malformed"], 0)
            rows = [json.loads(line) for line in (root / "out/gnss-1_sv-137_sig-0_freq-0.jsonl").read_text().splitlines()]
            self.assertEqual([r["offset"] for r in rows], [0, len(frame)])
            sources = json.loads((root / "out/sources.json").read_text())["sources"]
            self.assertEqual(sources[1]["stream_begin"], 19)
            self.assertTrue((root / "out/verified.json").exists())


if __name__ == "__main__":
    unittest.main()
