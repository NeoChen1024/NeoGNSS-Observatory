# SPDX-License-Identifier: GPL-3.0-only
import unittest

from neognss_observatory.sbas_grid import COORDINATES, TECU_PER_M, HourlyGrid


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
        self.assertEqual([row["hour_gpst"] for row in rows], [0, 3600])
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


if __name__ == "__main__":
    unittest.main()
