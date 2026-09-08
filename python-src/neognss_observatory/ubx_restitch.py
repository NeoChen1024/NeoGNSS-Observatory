#!/usr/bin/env python
"""Conservative, provenance-preserving UBX archive reconstruction."""

import hashlib
import json
import mmap
import os
import struct
import sys
import uuid
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

import click
from tqdm import tqdm

from . import _native
from .gpst import label_ms as gpst_label
from .protocol import ProtocolWarnings, require_ubx

RECORD = struct.Struct("<QQqqQIIIi")
UNKNOWN = -(1 << 63)
EOE, PVT, INFERRED, NO_NAV, PARTIAL, NOISE, CONFLICT = (1, 2, 4, 8, 16, 32, 64)
RAWX = 128


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def write_plan(path, plan):
    with path.open("x") as stream:
        stream.write(
            json.dumps(
                {
                    "record_type": "header",
                    "schema": plan["schema"],
                    "time_scale": "GPST",
                    "time_origin": "1980-01-06 00:00:00 GPST",
                    "index_time_unit": "millisecond",
                    "gap_timeout_ms": plan["gap_timeout_ms"],
                    "byte_accounting": plan["byte_accounting"],
                }
            )
            + "\n"
        )
        for kind in ("sources", "joins", "artifacts", "events"):
            for record in plan[kind]:
                stream.write(json.dumps({"record_type": kind, **record}) + "\n")
        stream.write(
            json.dumps(
                {
                    "record_type": "complete",
                    "counts": {kind: len(plan[kind]) for kind in ("sources", "joins", "artifacts", "events")},
                }
            )
            + "\n"
        )


@dataclass(frozen=True)
class Epoch:
    begin: int
    end: int
    gpst_ms: int
    tow: int
    fingerprint: int
    flags: int
    frames: int
    nav: int
    week: int


@contextmanager
def read_index(path):
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
        if mapped[:8] != b"UBXIDX04" or (len(mapped) - 48) % RECORD.size:
            raise ValueError(f"Expected a millisecond UBXIDX04 index; rebuild the indexer and inventory: {path}")
        yield mapped


def epochs(path):
    with read_index(path) as mapped:
        for offset in range(48, len(mapped), RECORD.size):
            yield Epoch(*RECORD.unpack_from(mapped, offset))


def source_identity(path):
    st = path.stat()
    return {"path": str(path.resolve()), "size": st.st_size, "mtime_ns": st.st_mtime_ns}


def inventory_source(path, state_dir, tool_sha):
    identity = source_identity(path)
    key = hashlib.sha256(json.dumps([identity, tool_sha, "UBXIDX04"], sort_keys=True).encode()).hexdigest()
    directory = state_dir / key
    index_path = directory / "epochs.idx"
    summary_path = directory / "source.json"
    if summary_path.exists():
        summary = json.loads(summary_path.read_text())
        if summary["index_sha256"] != sha256(index_path):
            raise ValueError(f"Corrupt cached index: {index_path}")
        return summary
    directory.mkdir(exist_ok=True)
    temporary = directory / f"epochs.{uuid.uuid4().hex}.partial"
    with path.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
        index = _native.archive_index(mapped)
    with temporary.open("xb") as target:
        target.write(index)
    if source_identity(path) != identity:
        raise ValueError(f"Source changed during inventory: {path}")
    counts = Counter()
    first = last = previous = None
    for e in epochs(temporary):
        counts["records"] += 1
        counts["frames"] += e.frames
        if e.flags & NOISE:
            counts["excluded_bytes"] += e.end - e.begin
            if e.flags & 256:
                counts["skipped_protocol_frames"] += 1
                counts["skipped_protocol_bytes"] += e.end - e.begin
        elif e.gpst_ms == UNKNOWN:
            counts["untimed_bytes"] += e.end - e.begin
        else:
            first = e.gpst_ms if first is None else min(first, e.gpst_ms)
            last = e.gpst_ms if last is None else max(last, e.gpst_ms)
            # RAWX-only runs are quarantined measurement times, not NAV epochs.
            # Their receiver-clock offset cannot prove navigation time reversal.
            if e.nav:
                if previous is not None and e.gpst_ms < previous:
                    counts["time_reversals"] += 1
                previous = e.gpst_ms
        if e.flags & CONFLICT:
            counts["time_conflicts"] += 1
        if e.flags & PARTIAL:
            counts["partial_records"] += 1
    with read_index(temporary) as mapped:
        source_sha = mapped[8:40].hex()
    summary = {
        **identity,
        "sha256": source_sha,
        "indexer_schema": tool_sha,
        "index": str(index_path.resolve()),
        "index_sha256": sha256(temporary),
        "first_gpst_ms": first,
        "last_gpst_ms": last,
        **counts,
    }
    os.replace(temporary, index_path)
    write_json(summary_path, summary)
    return summary


def inventory(input_dir, state_dir, recursive=False):
    paths = sorted(path for path in input_dir.glob("**/*.ubx" if recursive else "*.ubx") if path.is_file())
    if not paths:
        raise ValueError("No expanded .ubx inputs")
    state_dir.mkdir(parents=True, exist_ok=True)
    tool_sha = _native.archive_schema
    sources = []
    with (
        ThreadPoolExecutor(max_workers=3) as pool,
        tqdm(total=sum(p.stat().st_size for p in paths), unit="B", unit_scale=True, desc="Inventory") as progress,
    ):
        tasks = [pool.submit(inventory_source, p, state_dir, tool_sha) for p in paths]
        for task in as_completed(tasks):
            source = task.result()
            ProtocolWarnings(source["path"], "ubx").update(source, final=True)
            sources.append(source)
            progress.update(source["size"])
            progress.set_postfix(files=len(sources), refresh=False)
    return sorted(sources, key=lambda s: (s["first_gpst_ms"] is None, s["first_gpst_ms"] or 0, s["path"]))


def run_restitch(input_dir, state_dir, recursive, output_dir, plan_only, gap_timeout, protocol="ubx"):
    """Prove overlaps, split at GPST days/NAV gaps, and publish verified outputs."""
    from .ubx_output import publish
    from .ubx_reconstruction import build_plan, segment_plan

    try:
        require_ubx(protocol, "UBX archive reconstruction")
        input_dir, state_dir, output_dir = (p.resolve() for p in (input_dir, state_dir, output_dir))
        if output_dir == input_dir or input_dir in output_dir.parents or output_dir in input_dir.parents:
            raise ValueError("Input and output directories must not overlap")
        sources = inventory(input_dir, state_dir, recursive=recursive)
        plan = segment_plan(build_plan(sources), gap_timeout_ms=round(gap_timeout * 1000))
        if plan_only:
            path = state_dir / f"plan-{uuid.uuid4().hex}.jsonl"
            write_plan(path, plan)
            click.echo(str(path))
        else:
            click.echo(json.dumps(publish(plan, output_dir)))
    except (OSError, ValueError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error
