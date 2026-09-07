# SPDX-License-Identifier: GPL-3.0-only
"""SBF input adapter; discard the envelope after obtaining SBAS and GPST."""

import os
import re

from tqdm import tqdm

from . import _native
from .protocol import ProtocolWarnings
from .sbas_frames import FrameStreams


def extract(root, sink, gap_ms):
    paths = sorted(
        p for p in root.rglob("*") if p.is_file() and (p.suffix.lower() in (".sbf", ".ubx") or re.fullmatch(r"\.\d{2}_", p.suffix))
    )
    if not paths:
        raise ValueError("No expanded raw .sbf, .ubx or .YY_ inputs")
    parser = _native.SbfParser(block_ids=[4020])
    streams = FrameStreams(sink, gap_ms)
    with tqdm(total=sum(p.stat().st_size for p in paths), desc="Extract SBF SBAS", unit="B", unit_scale=True) as progress:
        for path in paths:
            warnings = ProtocolWarnings(path, "sbf", parser.summary())
            with path.open("rb") as source:
                before = os.fstat(source.fileno())
                size = 0
                while data := source.read(4 * 1024 * 1024):
                    for block in parser.feed(data):
                        if "sbas" in block and "hex" in block["sbas"]:
                            streams.add(
                                block["prn"], block["gpst_ms"], block["sbas"], "receiver_message_time", block["receiver_crc_passed"]
                            )
                    warnings.update(parser.summary())
                    size += len(data)
                    progress.update(len(data))
                after = os.fstat(source.fileno())
            if size != before.st_size or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ValueError(f"Source changed while reading: {path}")
            warnings.update(parser.summary(), final=True)
    parser.finish()
    streams.finish()
    summary = parser.summary()
    if not summary["frames"]:
        raise ValueError("No valid SBF frames found in the selected inputs")
    return summary
