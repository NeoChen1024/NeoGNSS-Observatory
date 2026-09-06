# SPDX-License-Identifier: GPL-3.0-only
import unittest

from neognss_observatory.gpst import label, parse_hour


class GpstTests(unittest.TestCase):
    def test_origin_and_filename(self):
        self.assertEqual(label(0), "GPST-1980-01-06--00-00-00")
        self.assertEqual(parse_hour("1980-01-06T00"), 0)
        self.assertEqual(label(86400), "GPST-1980-01-07--00-00-00")

    def test_hour_roundtrip_has_no_leap_second_offset(self):
        hour = parse_hour("2025-04-01T00")
        self.assertEqual(label(hour), "GPST-2025-04-01--00-00-00")
        self.assertEqual(label(hour - 1), "GPST-2025-03-31--23-59-59")
        self.assertEqual(parse_hour("2025-04-02T00") - hour, 86400)

    def test_timezone_suffixes_are_not_accepted(self):
        for value in ("2025-04-01T00Z", "2025-04-01T00+00:00"):
            with self.assertRaises(ValueError):
                parse_hour(value)
