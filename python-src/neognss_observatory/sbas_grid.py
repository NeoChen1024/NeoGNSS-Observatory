#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Shared SBAS snapshot consumer and ParquetNEX replay CLI."""

import bisect
import json
from collections import defaultdict
from datetime import timedelta
from decimal import Decimal
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .cnex_import import ORIGIN, day_directories, latest_parts
from .research_output import staged_output, write_json


def controls(catalogs):
    """Delivery controls, not invented CommonNEX scientific records."""
    result = set()
    for batch in catalogs.get("events", ()):
        for kind, scope, stamp in zip(batch["kind"].to_pylist(), batch["scope"].to_pylist(), batch["gpst"].to_pylist()):
            if kind == "RECEIVER_RESTART":
                if stamp is None:
                    raise ValueError("Untimed receiver restart cannot be ordered for SBAS processing")
                result.add((stamp, True))
            elif kind == "EPOCH_COMPLETION" and scope == "NAVIGATION" and stamp is not None:
                result.add((stamp, False))
    for batch in catalogs.get("receiver-telemetry", ()):
        result.update((stamp, False) for stamp in batch["gpst"].to_pylist() if stamp is not None)
    return sorted(result, key=lambda c: (c[0], not c[1]))


class SbasGridProcessor:
    """One source-ordered processor for replay and live CommonNEX groups."""

    def __init__(self, setup_id, interval_s=3600, correction_age_s=600, mask_age_s=1200):
        self.native = _native.SbasGridProcessor(setup_id, interval_s, correction_age_s, mask_age_s)
        self.setup_id = setup_id
        self.progress = None
        self.closed = False

    def process(self, raw_bits=(), progress_controls=()):
        if self.closed:
            raise RuntimeError("SBAS processor is closed")
        batches = (
            raw_bits.to_batches()
            if isinstance(raw_bits, pa.Table)
            else (raw_bits,) if isinstance(raw_bits, pa.RecordBatch) else raw_bits
        )
        pending = iter(progress_controls)
        control = next(pending, None)
        output = defaultdict(list)

        def collect(values):
            for kind, value in values.items():
                output[kind].append(pa.record_batch(value))

        def apply(item):
            stamp, reset = item
            if reset:
                if self.progress is not None and stamp >= self.progress:
                    collect(self.native.advance(*divmod(int(stamp * 10**12), 10**12)))
                self.native.discontinuity()
                self.progress = None
            if self.progress is None or stamp > self.progress:
                ticks = int(stamp * 10**12)
                collect(self.native.advance(*divmod(ticks, 10**12)))
                self.progress = stamp

        def accept(batch):
            if not batch.num_rows:
                return
            collect(self.native.feed(batch))
            values = pc.drop_null(batch["nav_epoch_gpst"])
            if len(values):
                self.progress = values[-1].as_py()

        for batch in batches:
            # Controls are sparse epoch-level operations, not per-frame callbacks.
            stamps = batch["nav_epoch_gpst"].to_pylist()
            known = [(i, t) for i, t in enumerate(stamps) if t is not None]
            times = [t for _, t in known]
            begin = 0
            while control is not None and times and control[0] <= times[-1]:
                pos = bisect.bisect_left(times, control[0])
                stop = known[pos][0]
                if stop > begin:
                    accept(batch.slice(begin, stop - begin))
                apply(control)
                begin = stop
                control = next(pending, None)
            accept(batch.slice(begin))
        while control is not None:
            apply(control)
            control = next(pending, None)
        return dict(output)

    def feed(self, group):
        for batches in group.catalogs.values():
            for batch in batches:
                if "setup_id" in batch.schema.names and pc.any(pc.not_equal(batch["setup_id"], self.setup_id)).as_py():
                    raise ValueError("Mixed SBAS Setup")
        result = self.process(group.catalogs.get("raw-bits", ()), controls(group.catalogs))
        if group.notice and group.notice.startswith("discontinuity:"):
            self.native.discontinuity()
            self.progress = None
        return result

    def finish(self):
        """Do not extrapolate to a future snapshot or manufacture an EOF epoch."""
        self.closed = True
        return {}

    @property
    def diagnostics(self):
        return self.native.diagnostics


def station_batches(root):
    """Bounded per-day control index, source-ordered RawBits across day boundaries."""
    setup_id = json.loads((root / "setup.json").read_text())["setup_id"]
    for day in day_directories(root):
        catalogs = defaultdict(list)
        for name, columns in [("events", ["setup_id", "gpst", "kind", "scope"]), ("receiver-telemetry", ["setup_id", "gpst"])]:
            for path in latest_parts(day, name):
                with pq.ParquetFile(path) as source:
                    for batch in source.iter_batches(columns=columns, batch_size=65536):
                        if pc.any(pc.not_equal(batch["setup_id"], setup_id)).as_py():
                            raise ValueError(f"Mixed SBAS control Setup: {path}")
                        catalogs[name].append(batch)
        pending = controls(catalogs)
        del catalogs
        times = [t for t, _ in pending]
        offset = 0
        for path in latest_parts(day, "raw-bits"):
            with pq.ParquetFile(path) as source:
                for batch in source.iter_batches(batch_size=65536):
                    known = pc.drop_null(batch["nav_epoch_gpst"])
                    end = bisect.bisect_right(times, known[-1].as_py(), offset) if len(known) else offset
                    yield batch, pending[offset:end]
                    offset = end
        if offset < len(pending):
            yield (), pending[offset:]


class SnapshotSink:
    """At most two open daily Parquet writers, no interval intermediates."""

    def __init__(self, output, interval_s, correction_age_s, mask_age_s):
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=False)
        self.day = None
        self.writers = {}
        self.rows = defaultdict(int)
        self.metadata = {
            b"time_scale": b"GPST",
            b"time_origin": b"1980-01-06 00:00:00 GPST",
            b"snapshot_interval_s": str(interval_s).encode(),
            b"correction_age_s": str(correction_age_s).encode(),
            b"mask_age_s": str(mask_age_s).encode(),
            b"aging_basis": b"RECEPTION_CONTEXT",
            b"mt0_policy": b"research: retain values; flag 60-second reception-context restriction; no safety assurance",
        }

    def add(self, output):
        days = sorted({int(t // 86400) for batches in output.values() for b in batches for t in b["snapshot_gpst"].to_pylist()})
        for day in days:
            if self.day is not None and day < self.day:
                raise ValueError("SBAS snapshot archive time moved backwards")
            if day != self.day:
                self.close()
                self.day = day
            directory = self.output / (ORIGIN + timedelta(days=day)).strftime("%Y/%m/%d")
            directory.mkdir(parents=True, exist_ok=True)
            for kind, batches in output.items():
                for batch in batches:
                    stamp = batch["snapshot_gpst"]
                    selected = batch.filter(
                        pc.and_(pc.greater_equal(stamp, Decimal(day * 86400)), pc.less(stamp, Decimal((day + 1) * 86400)))
                    )
                    if not selected.num_rows:
                        continue
                    if kind not in self.writers:
                        meta = {**self.metadata, b"sbas.catalog": kind.encode(), b"day_gpst": str(day * 86400).encode()}
                        schema = selected.schema.with_metadata(meta)
                        path = directory / f"{kind}.parquet.new"
                        self.writers[kind] = (
                            pq.ParquetWriter(path, schema, compression="zstd", compression_level=3, use_dictionary=True),
                            path,
                        )
                    self.writers[kind][0].write_batch(selected)
                    self.rows[kind] += selected.num_rows

    def close(self):
        for writer, path in self.writers.values():
            writer.close()
            path.rename(path.with_suffix(""))
        self.writers.clear()


@click.command()
@click.option(
    "--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True, help="CommonNEX station directory."
)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--snapshot-interval", type=click.IntRange(1), default=3600, show_default=True)
@click.option("--correction-age", type=click.IntRange(1), default=600, show_default=True)
@click.option("--mask-age", type=click.IntRange(1), default=1200, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace completed output while retaining its backup.")
@staged_output
def cli(input_dir, output, snapshot_interval, correction_age, mask_age):
    """Export per-source SBAS grid snapshots from CommonNEX to daily Parquet."""
    try:
        setup = json.loads((input_dir / "setup.json").read_text())
        processor = SbasGridProcessor(setup["setup_id"], snapshot_interval, correction_age, mask_age)
        sink = SnapshotSink(output, snapshot_interval, correction_age, mask_age)
        try:
            with tqdm(desc="SBAS RawBits", unit="row", unit_scale=True) as progress:
                for batch, events in station_batches(input_dir):
                    sink.add(processor.process(batch, events))
                    if isinstance(batch, pa.RecordBatch):
                        progress.update(batch.num_rows)
            processor.finish()
            sink.close()
        except Exception:
            # Close handles without publishing an incomplete current-day file.
            for writer, _ in sink.writers.values():
                writer.close()
            raise
        result = dict(status="complete", rows=dict(sink.rows), diagnostics=processor.diagnostics)
        write_json(output / "completed.json", result)
        click.echo(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
