# SPDX-License-Identifier: GPL-3.0-only
"""CommonNEX selection and bounded publication helpers for incremental STEC."""

import json
import os
import shutil
import uuid
from contextlib import contextmanager
from datetime import date, timedelta
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from .cnex_import import day_directories, latest_parts
from .setup_metadata import validate_setup

DAY_NS = 86400 * 10**9
EPOCH = date(1980, 1, 6)


def selection(root, start=None, end=None):
    setup = validate_setup(json.loads((root / "setup.json").read_text()))
    days = []
    for directory in day_directories(root):
        label = "-".join(directory.relative_to(root).parts)
        if (start and label < start) or (end and label >= end):
            continue
        obs = latest_parts(directory, "observations")
        events = latest_parts(directory, "events")
        if obs:
            days.append((label, obs, events))
    if not days:
        raise ValueError("No selected CommonNEX Observation partitions")
    return setup, days


def signature(path, root):
    stat = path.stat()
    return dict(path=str(path.relative_to(root)), bytes=stat.st_size, mtime_ns=stat.st_mtime_ns)


def observation_batches(paths, setup_id):
    for path in paths:
        with pq.ParquetFile(path) as source:
            metadata = source.schema_arrow.metadata or {}
            if metadata.get(b"commonnex.catalog") != b"observations" or metadata.get(b"setup_id") != setup_id.encode():
                raise ValueError(f"Unexpected CommonNEX observation identity: {path}")
            if metadata.get(b"time.scale") != b"GPST" or source.schema_arrow.field("gpst").type != pa.decimal128(38, 12):
                raise ValueError(f"Expected CommonNEX GPST decimal seconds: {path}")
            yield from source.iter_batches(batch_size=65536)


def validate_events(paths, setup_id):
    # Only implemented completion events are accepted. Future stateful event
    # kinds must receive explicit scientific mappings instead of being ignored.
    for path in paths:
        with pq.ParquetFile(path) as source:
            info = source.schema_arrow.metadata or {}
            if info.get(b"commonnex.catalog") != b"events" or info.get(b"setup_id") != setup_id.encode():
                raise ValueError(f"Unexpected CommonNEX Events identity: {path}")
            if info.get(b"time.scale") != b"GPST":
                raise ValueError(f"Expected GPST Events: {path}")
            for batch in source.iter_batches(batch_size=8192):
                if not pc.all(pc.fill_null(pc.equal(batch["setup_id"], setup_id), False)).as_py():
                    raise ValueError("Mixed Events Setup")
                selected = batch.filter(pc.fill_null(pc.not_equal(batch["scope"], "NAVIGATION"), True))
                if not len(selected):
                    continue
                if not pc.all(pc.fill_null(pc.equal(selected["kind"], "EPOCH_COMPLETION"), False)).as_py():
                    raise ValueError("Unsupported observation/stream Event; explicit processing mapping required")
                complete = pc.struct_field(selected["payload"], ["epoch_completion", "completion"])
                if not pc.all(pc.fill_null(pc.equal(complete, "COMPLETE"), False)).as_py():
                    raise ValueError("Incomplete CommonNEX observation context")


@contextmanager
def publication(target, rebuild=False):
    """Hard-link unchanged outputs; replace modified files only in staging.

    No reader/writer concurrency guarantee. The previous result is recoverable.
    """
    target = target.absolute()
    if target.is_symlink() or (target.exists() and not target.is_dir()):
        raise ValueError("STEC output must be a regular directory, not a symlink")
    if target.resolve() in (Path.cwd().resolve(), *Path.cwd().resolve().parents):
        raise ValueError("Output must not be the workspace or its parent")
    target.parent.mkdir(parents=True, exist_ok=True)
    stage = target.with_name(f".{target.name}.partial-{uuid.uuid4().hex}")
    if target.exists() and not rebuild:
        if any(p.is_symlink() for p in target.rglob("*")):
            raise ValueError("Symlinks are not allowed inside a STEC output")
        shutil.copytree(target, stage, copy_function=os.link)
    else:
        stage.mkdir()
    try:
        yield stage
    except BaseException:
        # Preserve interrupted work; the published directory remains intact.
        import click

        click.echo(f"Incomplete STEC output retained at {stage}", err=True)
        raise
    backup = target.with_name(f"{target.name}.backup-{uuid.uuid4().hex}") if target.exists() else None
    if backup:
        target.rename(backup)
    try:
        stage.rename(target)
    except BaseException:
        if backup:
            backup.rename(target)
        raise
    if backup:
        import click

        click.echo(f"Previous output retained at {backup}", err=True)


def replace_json(path, value):
    temporary = path.with_suffix(path.suffix + ".new")
    with temporary.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def replace_table(path, table):
    temporary = path.with_suffix(path.suffix + ".new")
    pq.write_table(table, temporary, compression="zstd", compression_level=3)
    temporary.replace(path)


class DailySamples:
    """Re-encode only a day receiving tail samples; preserve other daily files."""

    def __init__(self, root, metadata):
        self.root, self.metadata = root, metadata
        self.writer = self.day = self.path = self.temporary = None
        self.changed = set()

    def append(self, array):
        for day in np.unique(array["gpst_ns"] // DAY_NS):
            rows = array[array["gpst_ns"] // DAY_NS == day]
            table = pa.table({name: rows[name] for name in rows.dtype.names})
            table = table.replace_schema_metadata({b"ngo": json.dumps(self.metadata).encode()})
            if day != self.day:
                self.close()
                self.day = int(day)
                label = (EPOCH + timedelta(days=self.day)).isoformat()
                self.changed.add(label)
                self.path = self.root / "samples" / f"GPST-{label}.parquet"
                self.path.parent.mkdir(exist_ok=True)
                self.temporary = self.path.with_suffix(".parquet.new")
                self.writer = pq.ParquetWriter(self.temporary, table.schema, compression="zstd", compression_level=3)
                if self.path.exists():
                    with pq.ParquetFile(self.path) as old:
                        for batch in old.iter_batches():
                            self.writer.write_batch(batch)
            self.writer.write_table(table)

    def close(self):
        if self.writer:
            self.writer.close()
            self.temporary.replace(self.path)
            self.writer = None
            self.day = None
