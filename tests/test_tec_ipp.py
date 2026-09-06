# SPDX-License-Identifier: GPL-3.0-only
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from neognss_observatory.sbas_grid_render import render
from neognss_observatory.tec_ipp import (
    ArcTracker,
    hourly_display_arcs,
    phase_gf,
    station_lonlat,
)


def sample(time=0, phase1=100.0, lli1=0, **extra):
    row = dict(
        gpst=time,
        gps_week=2280,
        gps_tow=time,
        satellite="G01",
        code1="1C",
        code2="2X",
        frequency1=1575.42e6,
        frequency2=1227.60e6,
        phase1=phase1,
        phase2=80.0,
        lli1=lli1,
        lli2=0,
        geometry_ok=1,
        elevation=45,
        azimuth=30,
        ipp_latitude=25,
        ipp_longitude=120,
        mapping=1.5,
    )
    return dict(row, **extra)


class TecIppTests(unittest.TestCase):
    def test_hourly_display_rebases_each_arc_without_mutating_tracks(self):
        points = [
            dict(gpst=3605, arc_id="a", dstec_tecu=102),
            dict(gpst=3600, arc_id="a", dstec_tecu=100),
            dict(gpst=3610, arc_id="b", dstec_tecu=-30),
            dict(gpst=3615, arc_id="b", dstec_tecu=-31),
        ]
        arcs, references = hourly_display_arcs(points)
        self.assertEqual([p["display_dstec_tecu"] for p in arcs["a"]], [0, 2])
        self.assertEqual([p["display_dstec_tecu"] for p in arcs["b"]], [0, -1])
        self.assertEqual(references["a"], dict(gpst=3600, dstec_tecu=100))
        self.assertEqual(references["b"]["gpst"], 3610)
        self.assertNotIn("display_dstec_tecu", points[0])
        next_hour, _ = hourly_display_arcs([dict(gpst=7200, arc_id="a", dstec_tecu=120)])
        self.assertEqual(next_hour["a"][0]["display_dstec_tecu"], 0)

    def test_phase_sign_and_units(self):
        self.assertAlmostEqual(phase_gf(1575.42e6 / 299792458, 0, 1575.42e6, 1227.60e6), 1)
        tracker = ArcTracker()
        tracker.process(sample(), 20)
        point = tracker.process(sample(1, phase1=100.1), 20)
        self.assertGreater(point["dstec_tecu"], 0)
        self.assertAlmostEqual(point["dstec_tecu"], 0.181153, places=5)

    def test_hour_boundary_does_not_reset_reference(self):
        tracker = ArcTracker()
        first = tracker.process(sample(3599), 20)
        next_point = tracker.process(sample(3600, phase1=100.1), 20)
        self.assertEqual(first["arc_id"], next_point["arc_id"])
        self.assertGreater(next_point["dstec_tecu"], 0)

    def test_gap_and_lli_start_new_arcs(self):
        tracker = ArcTracker()
        first = tracker.process(sample(), 20)
        gap = tracker.process(sample(3), 20)
        slip = tracker.process(sample(4, lli1=1), 20)
        self.assertEqual(len({r["arc_id"] for r in (first, gap, slip)}), 3)
        self.assertEqual(gap["arc_start_reason"], "epoch_gap")
        self.assertEqual(slip["arc_start_reason"], "lli_slip")

    def test_half_cycle_and_missing_geometry_break_arcs(self):
        for extra in (dict(lli1=2), dict(geometry_ok=0), dict(phase1=0)):
            tracker = ArcTracker()
            first = tracker.process(sample(), 20)
            self.assertIsNone(tracker.process(sample(time=1, **extra), 20))
            next_point = tracker.process(sample(2), 20)
            self.assertNotEqual(first["arc_id"], next_point["arc_id"])

    def test_large_gf_jump_is_marked_candidate(self):
        tracker = ArcTracker()
        tracker.process(sample(), 20)
        point = tracker.process(sample(1, phase1=110), 20)
        self.assertEqual(point["arc_start_reason"], "gf_jump_candidate")
        self.assertEqual(point["dstec_tecu"], 0)

    def test_station_position_uses_geodetic_latitude(self):
        lat = np.deg2rad(45)
        e2 = 6.6943799901413165e-3
        n = 6378137 / np.sqrt(1 - e2 * np.sin(lat) ** 2)
        lon, result = station_lonlat((n * np.cos(lat), 0, n * (1 - e2) * np.sin(lat)))
        self.assertAlmostEqual(result, 45)
        self.assertEqual(lon, 0)

    def test_parallel_sbas_png_matches_serial_pixels(self):
        coast = Path(__file__).parents[1] / "contrib/natural-earth/ne_10m_coastline.zip"
        rows = [
            dict(
                gnssId=1, svId=137, sigId=0, freqId=0, hour_gpst=h, longitude=120, latitude=25, coverage=1, vtec_tecu=30 + h / 3600
            )
            for h in (0, 3600)
        ]
        with tempfile.TemporaryDirectory() as temporary:
            serial, parallel = Path(temporary) / "serial", Path(temporary) / "parallel"
            serial.mkdir()
            parallel.mkdir()
            a, extent_a = render(rows, coast, serial, 0, 100, 0.25, workers=1)
            b, extent_b = render(rows, coast, parallel, 0, 100, 0.25, workers=2)
            self.assertEqual(extent_a, extent_b)
            self.assertEqual(a, b)
            for first, second in zip(a, b):
                with Image.open(serial / first["path"]) as x, Image.open(parallel / second["path"]) as y:
                    np.testing.assert_array_equal(np.asarray(x), np.asarray(y))


if __name__ == "__main__":
    unittest.main()
