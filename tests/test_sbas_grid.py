# SPDX-License-Identifier: GPL-3.0-only
"""SBAS grid snapshot processing on synthetic SBAS L1 RawBits."""

import unittest
from decimal import Decimal

import pyarrow as pa

from neognss_observatory import _native
from neognss_observatory.sbas_grid import SbasGridProcessor

TECU_PER_M = 1575.42e6 * 1575.42e6 / (40.3 * 1e16)
SETUP = "test"


def raw_bits_schema():
    reader = _native.CnexObservationReader("sbf", SETUP, 0, 1, 0, 1)
    return pa.record_batch(reader.feed(b"")[2]).schema


SCHEMA = raw_bits_schema()


def crc24q(bits):
    crc = 0
    for bit in bits:
        feedback = ((crc >> 23) ^ bit) & 1
        crc = (crc << 1) & 0xFFFFFF
        if feedback:
            crc ^= 0x864CFB
    return crc


def encode(message_type, fields):
    """Build one 250-bit SBAS L1 message: preamble, type, fields, CRC-24Q."""
    bits = [0] * 250

    def put(offset, width, value):
        for i in range(width):
            bits[offset + i] = (value >> (width - 1 - i)) & 1

    put(0, 8, 0x53)
    put(8, 6, message_type)
    for offset, width, value in fields:
        put(offset, width, value)
    put(226, 24, crc24q(bits[:226]))
    body = bytearray(32)
    for i, bit in enumerate(bits):
        body[i // 8] |= bit << (7 - i % 8)
    return bytes(body)


def mask(iodi=1, positions=(1,), count=1, band=7):
    fields = [(14, 4, count), (18, 4, band), (22, 2, iodi)]
    fields += [(24 + position - 1, 1, 1) for position in positions]
    return encode(18, fields)


def correction(delay=8, iodi=1, band=7, block=0, givei=1):
    fields = [(14, 4, band), (18, 4, block), (217, 2, iodi), (22, 9, delay), (31, 4, givei)]
    fields += [(22 + 13 * i, 9, 511) for i in range(1, 15)]
    return encode(26, fields)


def rows(*messages):
    """RawBits rows (time, body) with passing independent CRC checks."""
    return pa.RecordBatch.from_pylist(
        [
            {
                "setup_id": SETUP,
                "nav_epoch_gpst": None if time is None else Decimal(time),
                "satellite_system": "S",
                "satellite_number": 131,
                "bitstream_source": ["SBAS_L1"],
                "signal_composition": "single",
                "message_family": "SBAS_L1",
                "body_format": "SBAS_L1_250_V1",
                "content_kind": "navigation_bits",
                "bit_length": 250,
                "body": body,
                "unit_kind": "message",
                "completeness": "complete",
                "checks": [
                    {
                        "origin": "independent",
                        "kind": "crc",
                        "scope": "message",
                        "result": "pass",
                        "evidence": "computed",
                        "source_field": None,
                    }
                ],
                "frame_index": index,
                "_archive_day": 0,
            }
            for index, (time, body) in enumerate(messages)
        ],
        schema=SCHEMA,
    )


def grid(output):
    return [row for batch in output.get("grid", ()) for row in batch.to_pylist()]


class SbasGridTest(unittest.TestCase):
    def test_coordinate_table_and_invalid_mask_bit(self):
        processor = SbasGridProcessor(SETUP)
        processor.process(rows((0, mask(band=0, positions=(1,))), (0, mask(band=8, positions=(200,)))))
        processor.process(rows((1, correction(band=0)), (1, correction(band=8))))
        cells = {(r["band"], r["mask_bit"]): (r["latitude"], r["longitude"]) for r in grid(processor.process(progress_controls=[(3600, False)]))}
        self.assertEqual(cells, {(0, 1): (-75.0, -180.0), (8, 200): (55.0, 175.0)})
        rejected = SbasGridProcessor(SETUP)
        rejected.process(rows((0, mask(band=8, positions=(201,)))))
        self.assertEqual(rejected.diagnostics["rejected_content"], 1)
        self.assertNotIn("mask_messages", rejected.diagnostics)

    def test_time_weighted_mean_across_snapshot(self):
        processor = SbasGridProcessor(SETUP, interval_s=3600, correction_age_s=600, mask_age_s=1200)
        first = processor.process(rows((3500, mask()), (3590, correction(8)), (3610, correction(16))))
        second = processor.process(progress_controls=[(7200, False)])
        (hour1,) = grid(first)
        (hour2,) = grid(second)
        self.assertEqual((hour1["snapshot_gpst"], hour2["snapshot_gpst"]), (Decimal(3600), Decimal(7200)))
        self.assertEqual(hour1["valid_duration_s"], Decimal(10))
        self.assertAlmostEqual(hour1["mean_vtec_tecu"], TECU_PER_M)
        # 10 s at 1 m before the second report, then 600 s at 2 m until correction expiry.
        self.assertEqual(hour2["valid_duration_s"], Decimal(610))
        self.assertAlmostEqual(hour2["mean_vtec_tecu"], (10 + 1200) / 610 * TECU_PER_M)
        self.assertEqual(hour2["status"], "EXPIRED")
        self.assertIsNone(hour2["vtec_tecu"])

    def test_expiry_not_monitored_and_mask_issue(self):
        processor = SbasGridProcessor(SETUP)
        processor.process(rows((0, mask()), (1, correction(8))))
        (row,) = grid(processor.process(progress_controls=[(3600, False)]))
        self.assertEqual(row["valid_duration_s"], Decimal(600))
        self.assertEqual(row["expiry_gpst"], Decimal(601))
        processor = SbasGridProcessor(SETUP)
        processor.process(rows((0, mask()), (1, correction(8, givei=15)), (2, mask(iodi=2)), (3, correction(8, iodi=1))))
        output = processor.process(progress_controls=[(10, False)])
        self.assertEqual(grid(output), [])
        self.assertEqual(processor.diagnostics["correction_without_mask"], 1)
        # The new IODI invalidated the NOT_MONITORED cell before it could expire.
        (row,) = grid(processor.process(progress_controls=[(3600, False)]))
        self.assertEqual(row["status"], "MASK_CHANGED")
        self.assertEqual(row["valid_duration_s"], Decimal(0))
        self.assertIsNone(row["mean_vtec_tecu"])

    def test_incomplete_multiband_mask_is_flagged(self):
        processor = SbasGridProcessor(SETUP)
        processor.process(rows((0, mask(count=2)), (1, correction())))
        (row,) = grid(processor.process(progress_controls=[(3600, False)]))
        self.assertFalse(row["mask_set_complete"])
        self.assertEqual(row["status"], "EXPIRED")


if __name__ == "__main__":
    unittest.main()
