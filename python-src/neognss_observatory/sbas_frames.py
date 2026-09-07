# SPDX-License-Identifier: GPL-3.0-only
"""Source-independent daily SBAS frames and explicit stream end records."""

from collections import defaultdict

import pyarrow as pa
import pyarrow.parquet as pq

from .gpst import label

SCHEMA = pa.schema(
    [
        ("gpst_ms", pa.int64()),
        ("stream_id", pa.int64()),
        ("frame_id", pa.int64()),
        ("constellation", pa.string()),
        ("prn", pa.int64()),
        ("signal", pa.string()),
        ("time_basis", pa.string()),
        ("kind", pa.string()),
        ("frame", pa.binary(32)),
        ("crc_valid", pa.bool_()),
        ("accepted", pa.bool_()),
    ],
    metadata={
        b"product": b"sbas_frames",
        b"schema_version": b"1",
        b"time_scale": b"GPST",
        b"time_origin": b"1980-01-06 00:00:00 GPST",
        b"frame_encoding": b"250 bits MSB-first; last 6 bits zero",
    },
)


class FrameSink:
    """Bounded shards prevent keeping every day's writer open."""

    def __init__(self, output):
        self.output = output
        self.staging = output / "staging"
        self.staging.mkdir()
        self.buffer, self.parts = defaultdict(list), defaultdict(list)
        self.count = self.frames = 0

    def add(self, row):
        day = row["gpst_ms"] // 86400000 * 86400000
        self.buffer[day].append(row)
        self.count += 1
        self.frames += row["kind"] == "frame"
        if self.count >= 8192:
            self.flush()

    def flush(self):
        for day, rows in self.buffer.items():
            path = self.staging / f"{day}-{len(self.parts[day])}.parquet"
            pq.write_table(pa.Table.from_pylist(rows, schema=SCHEMA), path, compression="zstd", compression_level=3)
            self.parts[day].append(path)
        self.buffer.clear()
        self.count = 0

    def finish(self):
        self.flush()
        daily = self.output / "daily"
        daily.mkdir()
        for day, parts in sorted(self.parts.items()):
            schema = SCHEMA.with_metadata({**SCHEMA.metadata, b"day_gpst_ms": str(day).encode()})
            with pq.ParquetWriter(
                daily / (label(day // 1000)[:15] + ".parquet"), schema, compression="zstd", compression_level=3
            ) as writer:
                for part in parts:
                    with pq.ParquetFile(part) as source:
                        for batch in source.iter_batches():
                            writer.write_table(pa.Table.from_batches([batch]).replace_schema_metadata(schema.metadata))
            for part in parts:
                part.unlink()
        self.staging.rmdir()
        return len(self.parts)


class FrameStreams:
    """Normalize identity/time before persistence; no wire envelope escapes."""

    def __init__(self, sink, gap_ms=0):
        self.sink, self.gap_ms = sink, gap_ms
        self.active = {}
        self.next_stream = self.next_frame = 0

    def close(self, key, end=None):
        identity, last = self.active.pop(key)
        time = last if end is None else end
        if time < last:
            raise ValueError("Stream end precedes final SBAS frame")
        self.sink.add(dict(identity, gpst_ms=time, kind="end", frame_id=None, frame=None, crc_valid=None, accepted=None))

    def finish(self, end=None):
        for key in list(self.active):
            self.close(key, end)

    def add(self, prn, time, message, time_basis, accepted=None):
        if not isinstance(time, int) or isinstance(time, bool) or time < 0:
            raise ValueError("SBAS frame has no valid GPST")
        key = ("SBAS", prn, "L1CA")
        if key in self.active:
            _, last = self.active[key]
            if time < last:
                raise ValueError("SBAS GPST reversed")
            if self.gap_ms and time - last > self.gap_ms:
                self.close(key)
        if key not in self.active:
            identity = dict(zip(("constellation", "prn", "signal"), key), stream_id=self.next_stream, time_basis=time_basis)
            self.next_stream += 1
            self.active[key] = identity, time
        identity, _ = self.active[key]
        data = bytearray.fromhex(message["hex"])
        if len(data) != 32:
            raise ValueError("Expected a complete SBAS L1 frame")
        data[-1] &= 0xC0
        self.sink.add(
            dict(
                identity,
                gpst_ms=time,
                kind="frame",
                frame_id=self.next_frame,
                frame=bytes(data),
                crc_valid=message["crc_valid"],
                accepted=accepted,
            )
        )
        self.next_frame += 1
        self.active[key] = identity, time
