# SPDX-License-Identifier: GPL-3.0-only
"""Bounded one-hour storage and JSONL acquisition for the realtime viewer."""

import json
import math
import os
import queue
import select
import threading
import time
from collections import deque
from dataclasses import dataclass
from decimal import ROUND_HALF_EVEN, Decimal, InvalidOperation

import numpy as np

DTYPE = np.dtype(
    [
        ("time", "i8"),
        ("segment", "i8"),
        ("arc", "i8"),
        ("satellite", "U4"),
        ("pair", "U5"),
        ("tec", "f8"),
        ("latitude", "f8"),
        ("longitude", "f8"),
    ]
)
HOUR_NS = 3600 * 10**9
MAX_LINE = 1024 * 1024


@dataclass(frozen=True)
class Epoch:
    setup: str
    time_ns: int
    samples: np.ndarray


def parse_epoch(line):
    row = json.loads(line)
    if not isinstance(row, dict):
        raise ValueError("Expected an epoch object")
    if not isinstance(row.get("setup_id"), str) or not row["setup_id"]:
        raise ValueError("Missing setup_id")
    try:
        t = Decimal(row["gpst"])
        if not t.is_finite() or t < 0:
            raise ValueError("Invalid GPST")
        ns = int((t * 10**9).to_integral_value(rounding=ROUND_HALF_EVEN))
        if ns > np.iinfo(np.int64).max:
            raise ValueError("GPST outside viewer nanosecond range")
    except (InvalidOperation, TypeError, KeyError) as error:
        raise ValueError("Expected decimal GPST seconds") from error
    values = row["samples"]
    if not isinstance(values, list) or len(values) > 4096:
        raise ValueError("Expected at most 4096 samples per epoch")
    segment = row.get("segment", 0)
    if type(segment) is not int or not 0 <= segment <= np.iinfo(np.int64).max:
        raise ValueError("Invalid segment")
    data = np.empty(len(values), dtype=DTYPE)
    for i, s in enumerate(values):
        sat, signals, arc = s["satellite"], s["signals"], s["arc_id"]
        if not isinstance(sat, str) or len(sat) not in (3, 4) or sat[0] not in "GJEC" or not sat[1:].isdigit():
            raise ValueError("Invalid satellite identity")
        if not isinstance(signals, list) or len(signals) != 2 or any(not isinstance(v, str) or len(v) != 2 for v in signals):
            raise ValueError("Expected two exact signal codes")
        if type(arc) is not int or not 0 <= arc <= np.iinfo(np.int64).max:
            raise ValueError("Invalid arc_id")
        tec, lat, lon = (float(s[k]) for k in ("relative_stec_tecu", "ipp_latitude_deg", "ipp_longitude_deg"))
        if not all(math.isfinite(v) for v in (tec, lat, lon)) or not -90 <= lat <= 90 or not -180 <= lon <= 180:
            raise ValueError("Invalid TEC or IPP coordinates")
        data[i] = (ns, segment, arc, sat, "/".join(signals), tec, lat, lon)
    data.flags.writeable = False
    return Epoch(row["setup_id"], ns, data)


class Window:
    def __init__(self, max_samples=1_000_000):
        self.frames = deque()
        self.max_samples = max_samples
        self.count = 0
        self.latest = None
        self.setup = None
        self.trimmed = False
        self.reset_count = 0

    def append(self, epoch):
        if self.setup != epoch.setup or (self.latest is not None and epoch.time_ns < self.latest):
            if self.setup is not None:
                self.reset_count += 1
            self.frames.clear()
            self.count = 0
            self.trimmed = False
        self.setup, self.latest = epoch.setup, epoch.time_ns
        self.frames.append(epoch)
        self.count += len(epoch.samples)
        while self.frames and self.frames[0].time_ns < self.latest - HOUR_NS:
            self.count -= len(self.frames.popleft().samples)
        while self.frames and (self.count > self.max_samples or len(self.frames) > 108000):
            self.count -= len(self.frames.popleft().samples)
            self.trimmed = True

    def array(self):
        return np.concatenate([e.samples for e in self.frames]) if self.frames else np.empty(0, dtype=DTYPE)


class JsonlReader(threading.Thread):
    """One bounded reader; the GUI never performs a blocking stdin read."""

    def __init__(self, source):
        super().__init__(name="stec-jsonl", daemon=True)
        self.source = source
        self.queue = queue.Queue(maxsize=16)
        self.stop = threading.Event()
        self.error = None
        self.done = False

    def lines(self):
        try:
            fd = self.source.fileno() if os.name == "posix" else None
        except (AttributeError, OSError):
            fd = None
        if fd is None:
            while not self.stop.is_set():
                line = self.source.readline(MAX_LINE + 1)
                if not line:
                    return
                yield line
            return
        pending = bytearray()
        while not self.stop.is_set():
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue
            chunk = os.read(fd, 65536)
            if not chunk:
                if pending:
                    yield bytes(pending)
                return
            pending.extend(chunk)
            while (end := pending.find(b"\n")) >= 0:
                yield bytes(pending[: end + 1])
                del pending[: end + 1]
                if self.stop.is_set():
                    return
            if len(pending) > MAX_LINE:
                raise ValueError("JSONL line exceeds 1 MiB")

    def run(self):
        line_number = 0
        try:
            for line in self.lines():
                line_number += 1
                if len(line) > MAX_LINE:
                    raise ValueError("JSONL line exceeds 1 MiB")
                if not line.strip():
                    continue
                epoch = parse_epoch(line)
                deadline = time.monotonic() + 5
                while not self.stop.is_set():
                    try:
                        self.queue.put(epoch, timeout=0.1)
                        break
                    except queue.Full:
                        if time.monotonic() > deadline:
                            raise BufferError("Viewer queue full for 5 seconds; input stopped")
        except (ValueError, KeyError, TypeError, OverflowError, OSError, BufferError) as error:
            self.error = f"Line {line_number}: {error}"
        finally:
            self.done = True
            self.source.close()
