#!/usr/bin/env python
"""Conservative, provenance-preserving UBX archive reconstruction."""

import hashlib
import json
import mmap
import os
import struct
import subprocess
import sys
import uuid
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

import click
from tqdm import tqdm

from .gpst import label as gpst_label

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
    gpst: int
    tow: int
    fingerprint: int
    flags: int
    frames: int
    nav: int
    week: int


@contextmanager
def read_index(path):
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
        if mapped[:8] != b"UBXIDX03" or (len(mapped) - 48) % RECORD.size:
            raise ValueError(f"Invalid index: {path}")
        yield mapped


def epochs(path):
    with read_index(path) as mapped:
        for offset in range(48, len(mapped), RECORD.size):
            yield Epoch(*RECORD.unpack_from(mapped, offset))


def source_identity(path):
    st = path.stat()
    return {"path": str(path.resolve()), "size": st.st_size, "mtime_ns": st.st_mtime_ns}


def inventory_source(path, state_dir, indexer, tool_sha):
    identity = source_identity(path)
    key = hashlib.sha256(json.dumps([identity, tool_sha], sort_keys=True).encode()).hexdigest()
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
    subprocess.run([str(indexer), str(path), str(temporary)], check=True)
    if source_identity(path) != identity:
        raise ValueError(f"Source changed during inventory: {path}")
    counts = Counter()
    first = last = previous = None
    for e in epochs(temporary):
        counts["records"] += 1
        counts["frames"] += e.frames
        if e.flags & NOISE:
            counts["excluded_bytes"] += e.end - e.begin
        elif e.gpst == UNKNOWN:
            counts["untimed_bytes"] += e.end - e.begin
        else:
            first = e.gpst if first is None else min(first, e.gpst)
            last = e.gpst if last is None else max(last, e.gpst)
            if previous is not None and e.gpst < previous:
                counts["time_reversals"] += 1
            previous = e.gpst
        if e.flags & CONFLICT:
            counts["time_conflicts"] += 1
        if e.flags & PARTIAL:
            counts["partial_records"] += 1
    with read_index(temporary) as mapped:
        source_sha = mapped[8:40].hex()
    summary = {
        **identity,
        "sha256": source_sha,
        "indexer_sha256": tool_sha,
        "index": str(index_path.resolve()),
        "index_sha256": sha256(temporary),
        "first_gpst": first,
        "last_gpst": last,
        **counts,
    }
    os.replace(temporary, index_path)
    write_json(summary_path, summary)
    return summary


def inventory(input_dir, state_dir, indexer, recursive=False):
    paths = sorted(path for path in input_dir.glob("**/*.ubx" if recursive else "*.ubx") if path.is_file())
    if not paths:
        raise ValueError("No expanded .ubx inputs")
    state_dir.mkdir(parents=True, exist_ok=True)
    tool_sha = sha256(indexer)
    sources = []
    with (
        ThreadPoolExecutor(max_workers=3) as pool,
        tqdm(total=sum(p.stat().st_size for p in paths), unit="B", unit_scale=True, desc="Inventory") as progress,
    ):
        tasks = [pool.submit(inventory_source, p, state_dir, indexer, tool_sha) for p in paths]
        for task in as_completed(tasks):
            source = task.result()
            sources.append(source)
            progress.update(source["size"])
            progress.set_postfix(files=len(sources), refresh=False)
    return sorted(sources, key=lambda s: (s["first_gpst"] is None, s["first_gpst"] or 0, s["path"]))


@click.group()
def cli():
    """Index and reconstruct raw UBX archives without modifying input files."""


def common_options(function):
    for option in reversed(
        [
            click.option("--input-dir", required=True, type=click.Path(exists=True, file_okay=False, path_type=Path)),
            click.option("--state-dir", required=True, type=click.Path(file_okay=False, path_type=Path)),
            click.option("--indexer", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path)),
            click.option("--recursive", is_flag=True, help="Include expanded .ubx files in subdirectories."),
        ]
    ):
        function = option(function)
    return function


@cli.command("inventory")
@common_options
def inventory_command(input_dir, state_dir, indexer, recursive):
    """Build resumable per-file frame/time indexes and report coverage."""
    try:
        sources = inventory(input_dir.resolve(), state_dir.resolve(), indexer.resolve(), recursive=recursive)
        for source in sources:
            click.echo(json.dumps(source))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise click.ClickException(str(error)) from error


@cli.command("run")
@common_options
@click.option("--output-dir", required=True, type=click.Path(file_okay=False, path_type=Path))
@click.option("--plan-only", is_flag=True, help="Write the validated plan in the state directory without touching output.")
def run_command(input_dir, state_dir, indexer, recursive, output_dir, plan_only):
    """Prove overlaps, split at GPST days/NAV gaps, and publish verified outputs."""
    from .ubx_output import publish
    from .ubx_reconstruction import build_plan, segment_plan

    try:
        input_dir, state_dir, indexer, output_dir = (p.resolve() for p in (input_dir, state_dir, indexer, output_dir))
        if output_dir == input_dir or input_dir in output_dir.parents or output_dir in input_dir.parents:
            raise ValueError("Input and output directories must not overlap")
        sources = inventory(input_dir, state_dir, indexer, recursive=recursive)
        plan = segment_plan(build_plan(sources))
        if plan_only:
            path = state_dir / f"plan-{uuid.uuid4().hex}.jsonl"
            write_plan(path, plan)
            click.echo(str(path))
        else:
            click.echo(json.dumps(publish(plan, output_dir, indexer)))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
