# SPDX-License-Identifier: GPL-3.0-only
"""CommonNEX selection and bounded publication helpers for incremental STEC."""

import json
import os
import shutil
import uuid
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from datetime import date, timedelta
from decimal import localcontext
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
        if obs or events:
            days.append((label, obs, events))
    if not any(obs for _, obs, _ in days):
        raise ValueError("No selected CommonNEX Observation partitions")
    return setup, days


def signature(path, root):
    stat = path.stat()
    return dict(path=str(path.relative_to(root)), bytes=stat.st_size, mtime_ns=stat.st_mtime_ns)


OBSERVATION_COLUMNS = [
    "setup_id",
    "gpst",
    "satellite_system",
    "satellite_number",
    "signal",
    "pseudorange_m",
    "carrier_phase_cycles",
    "code_quality.status",
    "phase_quality.status",
    "phase_tracking.half_cycle_ambiguity",
    "phase_tracking.half_cycle_subtracted",
    "phase_tracking.loss_of_lock",
    "phase_tracking.continuity_counter",
    "phase_tracking.continuity_counter_modulus",
    "phase_tracking.lock.lower_s",
    "receiver_corrections.code_smoothing_applied",
]


def _observation_batches(paths, setup_id):
    for path in paths:
        with pq.ParquetFile(path) as source:
            metadata = source.schema_arrow.metadata or {}
            if metadata.get(b"commonnex.catalog") != b"observations" or metadata.get(b"setup_id") != setup_id.encode():
                raise ValueError(f"Unexpected CommonNEX observation identity: {path}")
            if metadata.get(b"time.scale") != b"GPST" or source.schema_arrow.field("gpst").type != pa.decimal128(38, 12):
                raise ValueError(f"Expected CommonNEX GPST decimal seconds: {path}")
            yield from source.iter_batches(batch_size=65536, columns=OBSERVATION_COLUMNS)


def observation_batches(paths, setup_id):
    """One read owner and at most one prefetched batch; preserve input order."""
    source = _observation_batches(paths, setup_id)
    end = object()
    with ThreadPoolExecutor(max_workers=1, thread_name_prefix="stec-read") as pool:
        future = pool.submit(next, source, end)
        try:
            while (batch := future.result()) is not end:
                future = pool.submit(next, source, end)
                yield batch
        finally:
            # A running generator must finish its current next() before close().
            future.cancel()
            pool.shutdown(wait=True, cancel_futures=True)
            source.close()


def validate_events(paths, setup_id):
    """Validate context and return unique timed receiver-restart boundaries.

    Only the sparse restart rows become Python objects; completion validation
    remains columnar. GPST quantization matches the native observation reader.
    """
    restarts = {}
    for path in paths:
        with pq.ParquetFile(path) as source:
            info = source.schema_arrow.metadata or {}
            if info.get(b"commonnex.catalog") != b"events" or info.get(b"setup_id") != setup_id.encode():
                raise ValueError(f"Unexpected CommonNEX event identity: {path}")
            if info.get(b"time.scale") != b"GPST" or source.schema_arrow.field("gpst").type != pa.decimal128(38, 12):
                raise ValueError(f"Expected decimal GPST Events: {path}")
            for batch in source.iter_batches(batch_size=65536):
                if not pc.all(pc.fill_null(pc.equal(batch["setup_id"], setup_id), False)).as_py():
                    raise ValueError(f"Mixed Events Setup: {path}")
                selected = batch.filter(pc.fill_null(pc.not_equal(batch["scope"], "NAVIGATION"), True))
                if not len(selected):
                    continue
                completion = pc.fill_null(
                    pc.and_(pc.equal(selected["kind"], "EPOCH_COMPLETION"), pc.equal(selected["scope"], "OBSERVATION")), False
                )
                restart = pc.fill_null(
                    pc.and_(pc.equal(selected["kind"], "RECEIVER_RESTART"), pc.equal(selected["scope"], "RECEIVER")), False
                )
                unsupported = selected.filter(pc.invert(pc.or_(completion, restart)))
                if len(unsupported):
                    row = unsupported.slice(0, 1).to_pylist()[0]
                    raise ValueError(f"Unsupported STEC Event {row['scope']}/{row['kind']} at {row['gpst']}: {path}")
                complete = selected.filter(completion)
                if len(complete):
                    valid = pc.and_(
                        pc.equal(pc.struct_field(complete["payload"], ["epoch_completion", "completion"]), "COMPLETE"),
                        pc.equal(complete["applicability"], "EPOCH"),
                    )
                    valid = pc.and_(valid, pc.is_valid(complete["gpst"]))
                    if not pc.all(pc.fill_null(valid, False)).as_py():
                        raise ValueError(f"Incomplete CommonNEX observation context: {path}")
                for row in selected.filter(restart).to_pylist():
                    payload = row["payload"] or {}
                    reason = payload.get("restart_reason")
                    uptime = row["receiver_uptime_s"]
                    if (
                        row["applicability"] != "POINT"
                        or row["evidence"] != "INFERRED"
                        or row["end_gpst"] is not None
                        or payload.get("epoch_completion") is not None
                        or reason not in ("UPTIME_DECREASE", "GPST_UPTIME_OFFSET_JUMP")
                        or uptime is None
                        or not uptime.is_finite()
                        or uptime < 0
                    ):
                        raise ValueError(f"Invalid RECEIVER_RESTART evidence ({reason}): {path}")
                    gpst = row["gpst"]
                    if gpst is None:
                        raise ValueError(
                            f"Untimed RECEIVER_RESTART ({reason}) cannot be placed on the observation timeline: {path}"
                        )
                    if not gpst.is_finite() or gpst < 0:
                        raise ValueError(f"Invalid RECEIVER_RESTART GPST: {path}")
                    with localcontext() as ctx:
                        ctx.prec = 50
                        ticks = int(gpst * 10**12)
                    ns, remainder = divmod(ticks, 1000)
                    ns += int(remainder > 500 or (remainder == 500 and ns % 2))
                    if ns > 2**63 - 1:
                        raise ValueError(f"RECEIVER_RESTART outside native nanosecond range: {path}")
                    if ns in restarts and restarts[ns] != ticks:
                        raise ValueError(f"Distinct restart GPST values collide at nanosecond precision: {path}")
                    restarts[ns] = ticks
    return sorted(restarts)


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


def native_table(array, metadata):
    table = pa.table({name: array[name] for name in array.dtype.names if name != "system"})
    table = table.append_column("satellite_system", pa.array([chr(v) for v in array["system"]], type=pa.string()))
    pairs = metadata["pairs"]
    ks = np.array([p["meters_per_tecu"] for p in pairs])
    return table.append_column("meters_per_tecu", pa.array(ks[array["pair_id"]]))


class DailySamples:
    """Re-encode only a day receiving tail samples; preserve other daily files."""

    def __init__(self, root, metadata):
        self.root, self.metadata = root, metadata
        self.writer = self.day = self.path = self.temporary = None
        self.changed = set()

    def append(self, array):
        for day in np.unique(array["gpst_ns"] // DAY_NS):
            rows = array[array["gpst_ns"] // DAY_NS == day]
            table = native_table(rows, self.metadata)
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
