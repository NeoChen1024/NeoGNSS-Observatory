# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from neognss_observatory.sbas_map_video import (
    encode_command,
    load_images,
    validate_probe,
    video_filter,
)


def png_header(width, height):
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", width, height)


class SbasMapVideoTest(unittest.TestCase):
    def test_manifest_order_dimensions_and_checksums(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            records = []
            for name in ("later.png", "earlier.png"):
                path = root / name
                path.write_bytes(png_header(1800, 1200))
                records.append({"path": name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
            manifest = root / "images.json"
            manifest.write_text(json.dumps({"images": records}))
            images, dimensions = load_images(manifest)
            self.assertEqual([path.name for path, _ in images], ["later.png", "earlier.png"])
            self.assertEqual(dimensions, (1800, 1200))

    def test_even_dimensions_are_not_rescaled_or_padded(self):
        filter_graph, padded = video_filter(1800, 1200)
        self.assertEqual(filter_graph, "format=nv12,hwupload")
        self.assertFalse(padded)
        command, _, _ = encode_command("ffmpeg", "0", "frame-%08d.png", "video.mp4", 1800, 1200, 5, 24, "Map", 10)
        self.assertIn("hevc_vulkan", command)
        self.assertIn("hvc1", command)
        self.assertIn("+faststart", command)

    def test_odd_dimensions_are_padded_to_nv12_alignment(self):
        filter_graph, padded = video_filter(1799, 1199)
        self.assertTrue(padded)
        self.assertTrue(filter_graph.startswith("pad="))

    def test_probe_validation(self):
        probe = {
            "streams": [
                {
                    "codec_name": "hevc",
                    "codec_tag_string": "hvc1",
                    "width": 1800,
                    "height": 1200,
                    "r_frame_rate": "5/1",
                    "avg_frame_rate": "5/1",
                    "nb_read_packets": "10",
                }
            ],
            "format": {"duration": "2.0"},
        }
        validate_probe(probe, 10, 1800, 1200, 5, False)


if __name__ == "__main__":
    unittest.main()
