# SPDX-License-Identifier: GPL-3.0-only
import json
import tempfile
import unittest
from pathlib import Path
from subprocess import CompletedProcess
from unittest.mock import patch

from click.testing import CliRunner

from neognss_observatory.sbf_rinex import (
    cli,
    conversion_command,
    convert,
    observation_summary,
)


class SbfRinexTests(unittest.TestCase):
    def test_non_gps_observation_time_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.obs"
            path.write_text(f"{'':48}GLO{'':9}TIME OF FIRST OBS\n{'':60}END OF HEADER\n")
            with self.assertRaisesRegex(ValueError, "requires explicit GPS"):
                observation_summary(path)

    def test_conversion_records_outputs_and_observation_inventory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input.sbf"
            source.write_bytes(b"test source")

            def run(command, cwd, **kwargs):
                (cwd / "input.25O").write_text(
                    f"{'':48}GPS{'':9}TIME OF FIRST OBS\n" f"{'':60}END OF HEADER\n> 2025 04 01 00 00 0.0000000  0 1\nG01  1\n"
                )
                return CompletedProcess(command, 0)

            with (
                patch("neognss_observatory.sbf_rinex.subprocess.run", side_effect=run),
                patch("neognss_observatory.sbf_rinex.platform.platform", return_value="test platform"),
            ):
                result = convert((source, root / "output", source, "4.01", "test", "test hash"))
            manifest = json.loads((root / "output/conversion.json").read_text())
            self.assertEqual(result["artifacts"], 1)
            self.assertEqual(manifest["status"], "complete")
            self.assertEqual(manifest["observation_summary"]["input.25O"]["epochs"], 1)
            self.assertEqual(source.read_bytes(), b"test source")

    def test_preservation_flags_do_not_resample_or_filter(self):
        args = conversion_command(Path("/tool"), Path("/source.25_"), "4.01")
        for flag in ("-R401", "-nOPBM", "-s", "-D", "-X", "-c"):
            self.assertIn(flag, args)
        for flag in ("-i", "-x", "-I", "-E", "-noevent", "-U"):
            self.assertNotIn(flag, args)

    def test_existing_output_is_not_reused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input.sbf"
            source.touch()
            with patch("neognss_observatory.sbf_rinex.subprocess.check_output", return_value="test"):
                result = CliRunner().invoke(cli, ["--source", str(source), "--tool", str(source), "--output", str(root)])
            self.assertNotEqual(result.exit_code, 0)
            self.assertFalse((root / "source-00000").exists())

    def test_epoch_inventory_excludes_events_and_reports_gaps(self):
        def header(body, label):
            return f"{body:<60}{label}\n"

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.obs"
            path.write_text(
                header("G    2 C1C L1C", "SYS / # / OBS TYPES")
                + header(" " * 48 + "GPS", "TIME OF FIRST OBS")
                + header("", "END OF HEADER")
                + "> 2025 04 01 00 00 0.0000000  0 1\nG01  1\n"
                + "> 2025 04 01 00 00 1.0000000  5 1\nCOMMENT\n"
                + "> 2025 04 01 00 00 2.0000000  0 1\nG01  2\n"
            )
            report = observation_summary(path)
            self.assertEqual(report["epochs"], 2)
            self.assertEqual(report["epoch_step_seconds"], {"2": 1})
            self.assertEqual(report["satellite_records"], {"G01": 2})
            self.assertEqual(report["observation_types"], {"G": ["C1C", "L1C"]})


if __name__ == "__main__":
    unittest.main()
