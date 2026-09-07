# SPDX-License-Identifier: GPL-3.0-only
"""Core receiver time, bias and restart calculations."""

import struct
import unittest

from neognss_observatory.receiver_clock import ClockTracker


def epoch(tow=100, bias=999000, drift=100, runtime=100, reset=False, week=2300, temperature=40, anchor=True):
    rows = []
    if anchor:
        rows.append((1, 0x20, struct.pack("<IihbBI", tow * 1000, -12, week, 18, 3, 2)))
        p = bytearray(48)
        struct.pack_into("<dH", p, 0, tow, week)
        p[12], p[13] = 2 if reset else 0, 1
        p[11] = 1
        rows.append((2, 0x15, p))
    rows.append((1, 0x22, struct.pack("<IiiII", tow * 1000, bias, drift, 2, 190)))
    if runtime is not None:
        p = bytearray(24)
        p[0] = 1
        struct.pack_into("<I", p, 8, runtime)
        struct.pack_into("<b", p, 18, temperature)
        rows.append((10, 0x39, p))
    rows.append((1, 0x61, struct.pack("<I", tow * 1000)))
    return rows


class ClockTests(unittest.TestCase):
    def setUp(self):
        self.samples, self.events, self.telemetry = [], [], []
        self.tracker = ClockTracker(self.samples.append, self.events.append, self.telemetry.append, max_gap=1.5)

    def feed(self, rows):
        for i, (cls, msg, p) in enumerate(rows):
            self.tracker.accept("input.ubx", i, cls, msg, len(p), p[:16] if cls == 2 else p)

    def test_units_temperature_and_confirmed_unwrap(self):
        self.feed(epoch())
        self.feed(epoch(tow=101, bias=-900, runtime=101, reset=True))
        row = self.samples[-1]
        self.assertEqual(row["clock_bias_unwrapped_ns"], 999100)
        self.assertEqual(row["clock_adjustment_total_ns"], -1000000)
        self.assertEqual(row["frequency_accuracy_ps_s"], 190)
        self.assertEqual(row["time_accuracy_ns"], 2)
        self.assertEqual(row["temperature_c"], 40)
        self.assertEqual(row["timegps_fTOW_ns"], -12)
        self.assertEqual(row["unwrap_quality"], "rawx_confirmed_adjustment")

    def test_runtime_equal_then_decrease_is_restart(self):
        self.feed(epoch())
        self.feed(epoch(tow=101, bias=-900, runtime=100))
        self.assertEqual(self.samples[-1]["receiver_session_id"], 0)
        self.feed(epoch(tow=102, bias=500, runtime=99))
        self.assertEqual(self.samples[-1]["receiver_session_id"], 1)
        self.assertEqual(self.samples[-1]["clock_adjustment_total_ns"], 0)
        self.assertEqual(self.tracker.counts["restarts"], 1)

    def test_positive_multiple_ms_adjustment(self):
        self.feed(epoch(bias=-2000000))
        self.feed(epoch(tow=101, bias=100, runtime=101))
        self.assertEqual(self.samples[-1]["clock_adjustment_total_ns"], 2000000)
        self.assertEqual(self.samples[-1]["clock_bias_unwrapped_ns"], -1999900)

    def test_gap_breaks_arc_not_session_and_drops_stale_temperature(self):
        self.feed(epoch())
        self.feed(epoch(tow=103, bias=100, runtime=None))
        self.assertEqual(self.samples[-1]["clock_arc_id"], 2)
        self.assertEqual(self.samples[-1]["receiver_session_id"], 0)
        self.assertNotIn("temperature_c", self.samples[-1])
        self.assertEqual(self.samples[-1]["unwrap_quality"], "gap")

    def test_fresh_temperature_after_gap_survives(self):
        self.feed(epoch())
        self.feed(epoch(tow=103, bias=100, runtime=103, temperature=41))
        self.feed(epoch(tow=104, bias=200, runtime=None))
        self.assertEqual(self.samples[-1]["temperature_c"], 41)
        self.assertEqual(self.samples[-1]["temperature_age_s"], 1)

    def test_noninteger_jump_is_not_unwrapped(self):
        self.feed(epoch())
        self.feed(epoch(tow=101, bias=500000))
        self.assertEqual(self.samples[-1]["unwrap_quality"], "unresolved_adjustment")
        self.assertEqual(self.samples[-1]["clock_adjustment_total_ns"], 0)

    def test_unassigned_time_retains_raw_clock(self):
        self.feed(epoch(anchor=False))
        self.assertIsNone(self.samples[-1]["gpst_ns"])
        self.assertEqual(self.samples[-1]["clock_bias_ns"], 999000)

    def test_week_boundary(self):
        self.feed(epoch(tow=604799))
        self.feed(epoch(tow=0, week=2301, bias=999100, runtime=101))
        self.assertEqual(self.samples[-1]["clock_arc_id"], 1)
        self.assertEqual(self.samples[1]["gpst_ns"] - self.samples[0]["gpst_ns"], 10**9)

    def test_rawx_only_anchor(self):
        self.feed([r for r in epoch() if r[:2] != (1, 0x20)])
        self.assertEqual(self.samples[0]["time_basis"], "NAV-CLOCK_iTOW_with_RXM-RAWX_nearest_second")
        self.assertEqual(self.samples[0]["gpst_ns"], (2300 * 604800 + 100) * 10**9)

    def test_nav_snap_restores_clock_millisecond_time(self):
        rows = epoch()
        for i, (cls, msg, p) in enumerate(rows):
            if cls == 1:
                p = bytearray(p)
                struct.pack_into("<I", p, 0, 99999)
                rows[i] = cls, msg, p
        self.feed(rows)
        self.assertEqual(len(self.samples), 1)
        self.assertEqual(self.samples[0]["gpst_ns"], (2300 * 604800 + 100) * 10**9 - 1000000)
        self.assertEqual(self.samples[0]["iTOW_ms"], 99999)

    def test_half_second_epochs_across_files_preserve_clock_arc(self):
        for index, itow in enumerate((195638001, 195638500, 195639000)):
            rows = [r for r in epoch(tow=195638, bias=1000 + index * 156, drift=312) if r[:2] != (2, 0x15)]
            for offset, (cls, msg, p) in enumerate(rows):
                p = bytearray(p)
                if cls == 1:
                    struct.pack_into("<I", p, 0, itow)
                self.tracker.accept(f"segment-{index}.ubx", offset, cls, msg, len(p), p)
        self.assertEqual(len(self.samples), 3)
        self.assertEqual(
            [r["gpst_ns"] for r in self.samples], [2300 * 604800 * 10**9 + t * 1000000 for t in (195638001, 195638500, 195639000)]
        )
        self.assertEqual([r["clock_arc_id"] for r in self.samples], [1, 1, 1])
        self.assertEqual(self.samples[-1]["temperature_age_s"], 0)

    def test_actual_duplicate_or_backward_time_still_fails_with_location(self):
        for tow in (100, 99):
            with self.subTest(tow=tow):
                self.setUp()
                self.feed(epoch(tow=100))
                with self.assertRaisesRegex(ValueError, "Non-increasing NAV-CLOCK GPST: input.ubx:.*previous input.ubx:"):
                    self.feed(epoch(tow=tow))

    def test_empty_rawx_does_not_conflict(self):
        rows = epoch()
        p = bytearray(16)
        p[13] = 1
        rows[1] = 2, 0x15, p
        self.feed(rows)
        self.assertEqual(len(self.samples), 1)
        self.assertEqual(self.samples[0]["gpst_ns"], (2300 * 604800 + 100) * 10**9)

    def test_nav_snap_week_carry(self):
        rows = [r for r in epoch(tow=0, week=2301) if r[:2] != (2, 0x15)]
        for i, (cls, msg, p) in enumerate(rows):
            if cls == 1:
                p = bytearray(p)
                struct.pack_into("<I", p, 0, 604799999)
                if msg == 0x20:
                    struct.pack_into("<h", p, 8, 2300)
                rows[i] = cls, msg, p
        self.feed(rows)
        self.assertEqual(self.samples[0]["gpst_ns"], 2301 * 604800 * 10**9 - 1000000)

    def test_exact_week_end_nav_matches_new_week_rawx(self):
        rows = epoch(tow=0, week=2301)
        for i, (cls, msg, p) in enumerate(rows):
            if cls == 1:
                p = bytearray(p)
                struct.pack_into("<I", p, 0, 604800000)
                if msg == 0x20:
                    struct.pack_into("<h", p, 8, 2300)
                rows[i] = cls, msg, p
        self.feed(rows)
        self.assertEqual(len(self.samples), 1)
        self.assertEqual(self.samples[0]["gpst_ns"], 2301 * 604800 * 10**9)
        self.assertEqual(self.samples[0]["iTOW_ms"], 604800000)

    def test_itow_beyond_week_reports_location(self):
        with self.assertRaisesRegex(ValueError, "604800001.*input.ubx:42"):
            self.tracker.accept("input.ubx", 42, 1, 0x61, 4, struct.pack("<I", 604800001))

    def test_unresolved_rawx_flag_breaks_arc(self):
        self.feed(epoch())
        self.feed(epoch(tow=101, bias=999100, runtime=101, reset=True))
        self.assertEqual(self.samples[-1]["clock_arc_id"], 2)

    def test_statistics(self):
        self.feed(epoch(bias=0, drift=10, temperature=30))
        self.feed(epoch(tow=101, bias=10, drift=20, temperature=40))
        summary = self.tracker.summary()
        self.assertEqual(summary["temperature_drift_pearson_r"], 1)
        self.assertEqual(summary["ranges"]["clock_drift_ns_s"], {"min": 10, "max": 20})

    def test_unknown_monitor_version_rejected(self):
        with self.assertRaisesRegex(ValueError, "msgVer"):
            self.tracker.accept("input", 0, 10, 0x39, 24, bytes(24))

    def test_conflicting_anchors_rejected(self):
        rows = epoch()
        p = bytearray(rows[1][2])
        struct.pack_into("<H", p, 8, 2301)
        rows[1] = (2, 0x15, p)
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.feed(rows)
