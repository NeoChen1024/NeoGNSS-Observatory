# SPDX-License-Identifier: GPL-3.0-only
"""Bounded backward time association of independently stored receiver status."""

import numpy as np
import pyarrow.parquet as pq


class StatusJoin:
    def __init__(self, path, max_age):
        self.batches = iter(pq.ParquetFile(path).iter_batches(columns=["gpst_ns", "receiver_session_id", "temperature_c"]))
        self.pending = None
        self.last = None
        self.max_age_ns = max_age * 1e9
        self.previous_time = -1
        self.done = False

    def temperature(self, times, sessions):
        result = np.full(len(times), np.nan)
        if not len(times):
            return result
        parts = [self.last] if self.last is not None else []
        while True:
            if self.pending is None:
                if self.done:
                    break
                batch = next(self.batches, None)
                if batch is None:
                    self.done = True
                    break
                t = batch.column("gpst_ns").fill_null(-1).to_numpy()
                self.pending = np.rec.fromarrays(
                    [
                        t,
                        batch.column("receiver_session_id").to_numpy(),
                        batch.column("temperature_c").to_numpy(zero_copy_only=False),
                    ],
                    names="time,session,temperature",
                )[t >= 0]
                if not len(self.pending):
                    self.pending = None
                    continue
                if self.pending.time[0] < self.previous_time or np.any(np.diff(self.pending.time) < 0):
                    raise ValueError("Non-monotonic status timestamps")
                self.previous_time = int(self.pending.time[-1])
            stop = np.searchsorted(self.pending.time, times[-1], side="right")
            if stop:
                parts.append(self.pending[:stop])
            if stop < len(self.pending):
                self.pending = self.pending[stop:]
                break
            self.pending = None
        if not parts:
            return result
        status = np.concatenate(parts)
        self.last = status[-1:]
        indices = np.searchsorted(status["time"], times, side="right") - 1
        rows = status[np.maximum(indices, 0)]
        age = times - rows["time"]
        valid = (indices >= 0) & (age >= 0) & (age <= self.max_age_ns) & (sessions == rows["session"])
        result[valid] = rows["temperature"][valid]
        return result
