# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from neognss_observatory.sbas_grid import COORDINATES, TECU_PER_M, HourlyGrid
from neognss_observatory.sbas_grid_render import EraATimeMapper, aggregate, render
from neognss_observatory.ubx_restitch import RECORD


def message(message_type, content, status="decoded"):
    return {"status": status, "type": message_type, "content": content}


def mask(iodi=1, positions=(1,), count=1, band=7):
    return message(
        18,
        {
            "kind": "ionospheric_mask",
            "number_of_bands_raw": count,
            "band": band,
            "iodi": iodi,
            "active_mask_positions": list(positions),
        },
    )


def correction(delay=8, iodi=1, band=7, block=0, status="usable", givei=1):
    values = [
        {
            "active_mask_ordinal": i + 1,
            "delay_raw": delay if i == 0 else 511,
            "givei": givei if i == 0 else 0,
            "delay_m": delay * 0.125 if i == 0 else None,
            "status": status if i == 0 else "do_not_use",
        }
        for i in range(15)
    ]
    return message(26, {"kind": "ionospheric_delay", "band": band, "block": block, "iodi": iodi, "corrections": values})


class HourlyGridTest(unittest.TestCase):
    def test_complete_coordinate_table(self):
        self.assertEqual(len(COORDINATES), 2192)
        self.assertEqual(COORDINATES[0, 1], (-75, -180))
        self.assertEqual(COORDINATES[0, 28], (85, -180))
        self.assertEqual(COORDINATES[8, 200], (55, 175))
        self.assertEqual(COORDINATES[9, 72], (60, 175))
        self.assertEqual(COORDINATES[10, 181], (-85, -170))
        self.assertNotIn((8, 201), COORDINATES)

    def test_time_weighted_average_crosses_hour(self):
        state = HourlyGrid()
        state.process(3500, mask())
        state.process(3590, correction(8))
        state.process(3610, correction(16))
        state.finish(3620)
        rows = list(state.rows())
        self.assertEqual([row["hour_utc"] for row in rows], [0, 3600])
        self.assertEqual([row["valid_seconds"] for row in rows], [10, 20])
        self.assertAlmostEqual(rows[0]["vtec_tecu"], TECU_PER_M)
        self.assertAlmostEqual(rows[1]["vtec_tecu"], 1.5 * TECU_PER_M)

    def test_expiry_invalid_values_and_mask_generation(self):
        state = HourlyGrid()
        state.process(0, mask())
        state.process(1, correction(8))
        state.finish(1000)
        self.assertEqual(list(state.rows())[0]["valid_seconds"], 600)
        state = HourlyGrid()
        state.process(0, mask())
        state.process(1, correction(8, status="not_monitored", givei=15))
        state.process(2, mask(iodi=2))
        state.process(3, correction(8, iodi=1))
        state.finish(10)
        self.assertEqual(list(state.rows()), [])
        self.assertEqual(state.diagnostics["not_monitored"], 1)
        self.assertEqual(state.diagnostics["correction_without_current_complete_mask"], 1)

    def test_incomplete_multiband_mask_is_not_used(self):
        state = HourlyGrid()
        state.process(0, mask(count=2))
        state.process(1, correction())
        state.finish(5)
        self.assertEqual(list(state.rows()), [])


class MappingTest(unittest.TestCase):
    def make_index(self, path, utc):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"UBXIDX02" + b"\0" * 40 + RECORD.pack(0, 100, utc, 0, 0, 1, 1, 1, 0))
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def make_reconstruction(self, root):
        reconstruction = root / "reconstruction"
        indexes = reconstruction / "provenance/indexes"
        indexes.mkdir(parents=True)
        records = []
        for number, utc in enumerate((3590, 3600)):
            source = f"source-{number}"
            digest = self.make_index(indexes / f"sha-{number}.idx", utc)
            records.append({"record_type": "sources", "path": source, "sha256": f"sha-{number}", "index_sha256": digest})
            records.append(
                {
                    "record_type": "artifacts",
                    "name": f"segment-{number}.ubx",
                    "kind": "utc_segment",
                    "start_utc": utc,
                    "end_utc": utc,
                    "size": 100,
                    "spans": [{"source": source, "begin": 0, "end": 100}],
                }
            )
        (reconstruction / "plan.jsonl").write_text("".join(json.dumps(record) + "\n" for record in records))
        return reconstruction

    def test_artifact_offset_uses_payload_epoch_index(self):
        with tempfile.TemporaryDirectory() as temp:
            mapper = EraATimeMapper(self.make_reconstruction(Path(temp)))
            try:
                self.assertEqual(mapper.artifact_cursor("segment-1.ubx").utc(50), 3600)
            finally:
                mapper.close()

    def test_aggregate_retains_mask_across_file_boundary(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reconstruction = self.make_reconstruction(root)
            extraction = root / "extraction"
            group = extraction / "group-00000"
            group.mkdir(parents=True)
            (extraction / "completed.json").write_text(json.dumps({"status": "complete", "continuous_groups": 1}))
            (extraction / "groups.jsonl").write_text(json.dumps({"group": "group-00000"}) + "\n")
            sources = [
                {"name": "segment-0.ubx", "stream_begin": 0, "stream_end": 100, "start_utc": 3590, "end_utc": 3590},
                {"name": "segment-1.ubx", "stream_begin": 100, "stream_end": 200, "start_utc": 3600, "end_utc": 3600},
            ]
            (group / "sources.json").write_text(json.dumps({"sources": sources, "start_utc": 3590, "end_utc": 3600}))
            rows = [
                {"offset": 50, "gnssId": 1, "svId": 137, "sigId": 0, "freqId": 0, "sbas": mask()},
                {"offset": 150, "gnssId": 1, "svId": 137, "sigId": 0, "freqId": 0, "sbas": correction(8)},
            ]
            (group / "gnss-1_sv-137_sig-0_freq-0.jsonl").write_text(
                "".join(json.dumps(row, separators=(",", ":")) + "\n" for row in rows)
            )
            averages, diagnostics, messages = aggregate(extraction, reconstruction)
            self.assertEqual(len(averages), 1)
            self.assertEqual(averages[0]["hour_utc"], 3600)
            self.assertEqual(averages[0]["valid_seconds"], 1)
            self.assertEqual(messages, {"18": 1, "26": 1})
            self.assertFalse(diagnostics)

    def test_renderer_writes_png_and_manifest(self):
        coast = Path(__file__).parents[1] / "contrib/natural-earth/ne_110m_coastline.zip"
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            rows = [
                {
                    "gnssId": 1,
                    "svId": 137,
                    "sigId": 0,
                    "freqId": 0,
                    "hour_utc": 3600,
                    "band": 7,
                    "mask_bit": 1,
                    "latitude": 25,
                    "longitude": 120,
                    "vtec_tecu": 42.0,
                    "valid_seconds": 3600,
                    "coverage": 1.0,
                }
            ]
            manifest, extent = render(rows, coast, output, 0, 100, 0.25)
            self.assertEqual(len(manifest), 1)
            image = output / manifest[0]["path"]
            self.assertEqual(image.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(hashlib.sha256(image.read_bytes()).hexdigest(), manifest[0]["sha256"])
            self.assertEqual(extent, (110, 130, 10, 40))


if __name__ == "__main__":
    unittest.main()
