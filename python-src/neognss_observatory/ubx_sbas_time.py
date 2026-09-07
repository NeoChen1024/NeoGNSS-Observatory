# SPDX-License-Identifier: GPL-3.0-only
"""UBX input-boundary mapping from reconstruction offsets to GPST."""

import bisect
import hashlib
import json
import mmap
from collections import OrderedDict
from pathlib import Path

from .ubx_restitch import RECORD, UNKNOWN

INDEX_HEADER_SIZE = 48


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class EpochIndex:
    def __init__(self, path, expected_sha256):
        self.path = path
        if sha256(path) != expected_sha256:
            raise ValueError(f"Reconstruction epoch index checksum mismatch: {path}")
        self.stream = path.open("rb")
        try:
            self.data = mmap.mmap(self.stream.fileno(), 0, access=mmap.ACCESS_READ)
        except BaseException:
            self.stream.close()
            raise
        if self.data[:8] not in (b"UBXIDX03", b"UBXIDX04") or (len(self.data) - INDEX_HEADER_SIZE) % RECORD.size:
            self.close()
            raise ValueError(f"Invalid reconstruction epoch index: {path}")
        self.count = (len(self.data) - INDEX_HEADER_SIZE) // RECORD.size

    def record(self, number):
        row = list(RECORD.unpack_from(self.data, INDEX_HEADER_SIZE + number * RECORD.size))
        if self.data[:8] == b"UBXIDX04" and row[2] != UNKNOWN:
            row[2] /= 1000  # Rendering uses GPST seconds, not index milliseconds.
        return row

    def locate(self, offset, hint=None):
        if hint is not None:
            begin, end, gpst, *_ = self.record(hint)
            if begin <= offset < end:
                return hint, gpst
            if offset >= end and hint + 1 < self.count:
                begin, end, gpst, *_ = self.record(hint + 1)
                if begin <= offset < end:
                    return hint + 1, gpst
        low, high = 0, self.count
        while low < high:
            middle = (low + high) // 2
            if self.record(middle)[0] <= offset:
                low = middle + 1
            else:
                high = middle
        number = low - 1
        if number < 0:
            raise ValueError(f"Offset precedes epoch index: {self.path}:{offset}")
        begin, end, gpst, *_ = self.record(number)
        if not begin <= offset < end or gpst == UNKNOWN:
            raise ValueError(f"SBAS offset has no payload-derived GPST epoch: {self.path}:{offset}")
        return number, gpst

    def close(self):
        self.data.close()
        self.stream.close()


class EraATimeMapper:
    def __init__(self, reconstruction, max_open_indexes=16):
        if max_open_indexes < 1:
            raise ValueError("Index cache capacity must be positive")
        self.max_open_indexes = max_open_indexes
        self.reconstruction = reconstruction
        self.source_indexes = {}
        self.indexes = OrderedDict()
        self.artifacts = {}
        self.time_overrides = {}
        with (reconstruction / "plan.jsonl").open() as stream:
            for line in stream:
                record = json.loads(line)
                if record.get("record_type") == "sources":
                    path = reconstruction / "provenance/indexes" / (record["sha256"] + ".idx")
                    self.source_indexes[record["path"]] = (path, record["index_sha256"])
                elif record.get("record_type") == "artifacts" and record.get("kind") == "gpst_segment":
                    self.artifacts[record["name"]] = record
                elif (
                    record.get("record_type") == "joins"
                    and record.get("kind") == "split_epoch_continuation"
                    and "anchor_gpst_ms" in record
                ):
                    self.time_overrides[record["previous"], record["previous_epoch_begin"]] = record["anchor_gpst_ms"] / 1000

    def index(self, source):
        if source not in self.indexes:
            # Each mapping can retain both a file and an mmap descriptor.
            # Evict before opening, independently of archive size/group count.
            if len(self.indexes) >= self.max_open_indexes:
                _, oldest = self.indexes.popitem(last=False)
                oldest.close()
            path, digest = self.source_indexes[source]
            self.indexes[source] = EpochIndex(path, digest)
        self.indexes.move_to_end(source)
        return self.indexes[source]

    def artifact_cursor(self, name):
        artifact = self.artifacts[name]
        ends, total = [], 0
        for span in artifact["spans"]:
            total += span["end"] - span["begin"]
            ends.append(total)
        if total != artifact["size"]:
            raise ValueError(f"Artifact span size mismatch: {name}")
        return ArtifactCursor(self, artifact, ends)

    def close(self):
        for index in self.indexes.values():
            index.close()
        self.indexes.clear()


class ArtifactCursor:
    def __init__(self, mapper, artifact, ends):
        self.mapper, self.artifact, self.ends = mapper, artifact, ends
        self.hints = {}

    def gpst(self, local_offset):
        if not 0 <= local_offset < self.artifact["size"]:
            raise ValueError(f"SBAS offset outside artifact: {self.artifact['name']}:{local_offset}")
        number = bisect.bisect_right(self.ends, local_offset)
        span = self.artifact["spans"][number]
        local_begin = 0 if number == 0 else self.ends[number - 1]
        original = span["begin"] + local_offset - local_begin
        index = self.mapper.index(span["source"])
        hint, gpst = index.locate(original, self.hints.get(span["source"]))
        gpst = self.mapper.time_overrides.get((span["source"], index.record(hint)[0]), gpst)
        self.hints[span["source"]] = hint
        if not self.artifact["start_gpst"] <= gpst <= self.artifact["end_gpst"]:
            raise ValueError(f"Mapped GPST outside artifact: {self.artifact['name']}")
        return gpst


class GroupOffsetMapper:
    def __init__(self, mapper, sources):
        self.sources = sources
        self.ends = [source["stream_end"] for source in sources]
        self.cursors = [mapper.artifact_cursor(source["name"]) for source in sources]

    def gpst(self, offset):
        number = bisect.bisect_right(self.ends, offset)
        if number == len(self.sources):
            raise ValueError(f"SBAS offset outside continuous group: {offset}")
        source = self.sources[number]
        if offset < source["stream_begin"]:
            raise ValueError(f"SBAS offset falls between group sources: {offset}")
        return self.cursors[number].gpst(offset - source["stream_begin"])
