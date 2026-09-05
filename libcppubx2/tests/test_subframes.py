# SPDX-License-Identifier: GPL-3.0-only
"""End-to-end wire fixtures; no archive or network access required."""

import hashlib
import json
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


def crc24q(data):
    # Byte-oriented reference, unlike the production bit-oriented decoder.
    crc = 0
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc


def sbas(mt, fields=(), preamble=0x53, padding=0):
    value = (preamble << 218) | (mt << 212)
    for offset, width, field in fields:
        shift = 226 - offset - width
        mask = (1 << width) - 1
        value = (value & ~(mask << shift)) | ((field & mask) << shift)
    crc = crc24q(value.to_bytes(29, "big"))  # Six leading zero bits.
    return ((value << 30) | (crc << 6) | padding).to_bytes(32, "big")


def wire(payload, cls=2, msg=0x13):
    body = bytes([cls, msg]) + struct.pack("<H", len(payload)) + payload
    a = b = 0
    for byte in body:
        a = (a + byte) & 255
        b = (b + a) & 255
    return b"\xb5\x62" + body + bytes([a, b])


def sfrbx(data, gnss=1, sv=137, sig=0, freq=0, chn=18, version=2, extra=()):
    words = struct.unpack(">" + "I" * (len(data) // 4), data) + tuple(extra)
    header = bytes([gnss, sv, sig, freq, len(words), chn, version, 224])
    return wire(header + struct.pack("<" + "I" * len(words), *words))


class SubframesTest(unittest.TestCase):
    def run_frames(self, data, success=True):
        with tempfile.TemporaryDirectory() as temp:
            src, dst = Path(temp) / "input.ubx", Path(temp) / "out"
            src.write_bytes(data)
            command = [os.environ["CPPUBX2_SUBFRAMES"], str(src), str(dst)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode == 0, success, result.stderr)
            if not success:
                self.assertFalse((dst / "summary.json").exists())
                return
            summary = json.loads(result.stdout)
            self.assertEqual(summary["source_sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(summary["source_bytes"], len(data))
            records = [json.loads(line) for path in sorted(dst.glob("gnss-*.jsonl")) for line in path.read_text().splitlines()]
            errors = [json.loads(s) for s in (dst / "errors.jsonl").read_text().splitlines()]
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            return summary, records, errors

    def test_reference_crc_and_content(self):
        self.assertEqual(crc24q(b"123456789"), 0xCDE703)
        cases = [
            sbas(0),
            sbas(63),
            sbas(1, [(14, 1, 1), (223, 1, 1), (224, 2, 3)]),
            *[sbas(mt, [(18, 12, -2048), (162, 12, 2047), (222, 4, 15)]) for mt in range(2, 6)],
            sbas(6, [(20, 2, 3), (222, 4, 14)]),
            sbas(7, [(14, 4, 15), (18, 2, 2), (222, 4, 13)]),
            sbas(9, [(39, 30, -1), (158, 18, -131072), (196, 10, -512), (218, 8, -128)]),
            sbas(18, [(14, 4, 2), (18, 4, 7), (22, 2, 3), (24, 1, 1), (224, 1, 1)]),
            sbas(26, [(14, 4, 7), (18, 4, 13), (22, 9, 511), (35, 9, 8), (44, 4, 15), (204, 9, 510), (217, 2, 3)]),
        ]
        summary, rows, _ = self.run_frames(b"".join(sfrbx(c) for c in cases))
        self.assertEqual(summary["sbas_status"], {"decoded": len(cases)})
        c = [r["sbas"]["content"] for r in rows]
        self.assertEqual(c[0]["kind"], "test_mode")
        self.assertEqual(c[1]["kind"], "null")
        self.assertEqual(c[2]["active_mask_positions"], [1, 210])
        self.assertEqual(c[2]["iodp"], 3)
        for i in range(4):
            self.assertEqual(c[3 + i]["first_mask_position"], 1 + 13 * i)
            self.assertEqual(c[3 + i]["satellites"][0]["correction_m"], -256)
            self.assertEqual(c[3 + i]["satellites"][-1]["correction_raw"], 2047)
            self.assertEqual(c[3 + i]["satellites"][-1]["udrei"], 15)
        self.assertEqual(c[7]["iodf"][-1], 3)
        self.assertEqual(c[7]["udrei"][-1], 14)
        self.assertEqual(len(c[7]["udrei"]), 51)
        self.assertEqual(c[8]["degradation_index"][-1], 13)
        self.assertEqual(c[9]["position_raw"][0], -1)
        self.assertEqual(c[9]["velocity_raw"][2], -131072)
        self.assertEqual(c[9]["acceleration_raw"][2], -512)
        self.assertEqual(c[9]["clock_drift_raw"], -128)
        self.assertEqual(c[10]["active_mask_positions"], [1, 201])
        self.assertEqual(c[11]["iodi"], 3)
        self.assertEqual(c[11]["corrections"][0]["status"], "do_not_use")
        self.assertIsNone(c[11]["corrections"][0]["delay_m"])
        self.assertEqual(c[11]["corrections"][1]["status"], "not_monitored")
        self.assertEqual(c[11]["corrections"][-1]["delay_m"], 63.75)

    def test_real_era_a_vectors(self):
        # 2023-05-19T04:42:58+0000.ubx, byte offsets 154996 and 194726.
        # Source SHA256 d62d764a70321ac566329a8fe9fa4b4b700f9806de2506432ac8959a41dc6623.
        delay = bytes.fromhex("9a69d027a0cd1078b7cb5e63e2af11706d42a910d07302ba0fd000600d7afc40")
        mask = bytes.fromhex("5348a30003ffc001ffc000fff0003ff0000ffc0003fc0000180000002b2e7000")
        _, rows, _ = self.run_frames(sfrbx(delay, extra=(0,)) + sfrbx(mask, extra=(0,)))
        for row in rows:
            self.assertTrue(row["sbas"]["crc_valid"])
            self.assertEqual(row["sbas"]["status"], "decoded")
            self.assertEqual(len(row["words"]), 9)
        self.assertEqual(rows[0]["sbas"]["content"]["corrections"][0]["delay_m"], 2.375)
        self.assertEqual(rows[1]["sbas"]["content"]["band"], 8)

    def test_errors_and_unsupported_are_not_decoded(self):
        corrupt = bytearray(sbas(18))
        corrupt[3] ^= 1
        data = [bytes(corrupt), sbas(18, preamble=0), sbas(10), sbas(18, [(18, 4, 15)]), sbas(26, [(18, 4, 15)])]
        _, rows, _ = self.run_frames(b"".join(sfrbx(c) for c in data) + sfrbx(b"\0" * 28) + sfrbx(sbas(63), sig=1))
        self.assertEqual(
            [r["sbas"]["status"] for r in rows],
            [
                "invalid_crc",
                "invalid_preamble",
                "unsupported_message",
                "invalid_content",
                "invalid_content",
                "invalid_word_count",
                "unsupported_signal",
            ],
        )
        self.assertEqual(rows[2]["sbas"]["content"]["kind"], "unparsed")

    def test_routing_padding_noise_and_container_errors(self):
        data = sbas(63, preamble=0xC6, padding=63)
        frames = [
            sfrbx(data),
            sfrbx(data, chn=22, freq=9),
            sfrbx(data, sv=138),
            sfrbx(data, gnss=5, sv=2),
            sfrbx(data, gnss=6, sv=2, freq=3),
            sfrbx(data, gnss=6, sv=2, freq=4),
            sfrbx(data, gnss=99, sv=2),
            sfrbx(data, gnss=0, sv=2, sig=4),
        ]
        bad = bytearray(sfrbx(data))
        bad[-1] ^= 1
        extra = bytes(bad) + sfrbx(data, version=1) + wire(bytes([1, 137, 0, 0, 9, 0, 2, 0]))
        summary, rows, errors = self.run_frames(b"noise" + b"".join(frames) + extra + b"tail")
        self.assertEqual(len(summary["streams"]), 7)
        self.assertEqual(summary["malformed"], 3)
        self.assertEqual(summary["discarded_noise_bytes"], 9)
        self.assertEqual(len(errors), 5)
        sbas_rows = [r for r in rows if r["gnssId"] == 1]
        self.assertEqual(sbas_rows[0]["offset"], 5)
        self.assertEqual(sbas_rows[0]["sbas"]["padding_bits"], 63)
        self.assertEqual(sbas_rows[1]["freqId"], 9)
        self.assertEqual(next(r for r in rows if r["gnssId"] == 5)["prn"], 194)
        self.assertNotIn("sbas", next(r for r in rows if r["gnssId"] == 5))

    def test_truncated_input_does_not_complete(self):
        self.run_frames(sfrbx(sbas(63))[:-1], success=False)


if __name__ == "__main__":
    unittest.main()
