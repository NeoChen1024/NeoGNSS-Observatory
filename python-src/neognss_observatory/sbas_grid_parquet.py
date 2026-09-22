# SPDX-License-Identifier: GPL-3.0-only
"""Produce daily GPST grid intervals from protocol-neutral SBAS streams."""

import json
from collections import defaultdict
from datetime import date
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .batch_pipeline import prefetched
from .cnex_import import ORIGIN, day_directories, latest_parts
from .gpst import label
from .research_output import staged_output, write_json

SCHEMA = pa.schema(
    [
        (name, pa.int64())
        for name in (
            "start_gpst_ms",
            "end_gpst_ms",
            "satellite_number",
            "band",
            "mask_bit",
            "iodi",
            "givei",
            "frame_id",
            "stream_id",
        )
    ]
    + [(name, pa.float64()) for name in ("latitude", "longitude", "delay_m", "vtec_tecu")]
    + [(name, pa.string()) for name in ("satellite_system", "signal")],
    metadata={
        b"schema_version": b"3",
        b"time_scale": b"GPST",
        b"time_origin": b"1980-01-06 00:00:00 GPST",
        b"time_basis": b"input SBAS frame context; not transmit time",
        b"interval": b"[start_gpst_ms,end_gpst_ms)",
    },
)


class DailySink:
    """Bounded buffers, followed by streaming compaction to one file per day."""

    def __init__(self, output):
        self.output = output
        self.staging = output / "staging"
        self.staging.mkdir()
        self.buffer = defaultdict(list)
        self.count = 0
        self.total_rows = 0
        self.parts = defaultdict(list)

    def add(self, rows):
        if not len(rows):
            return
        # Expand midnight crossings in original row order, then split by day.
        start, end = rows["start_gpst_ms"], rows["end_gpst_ms"]
        counts = (end - 1) // 86400000 - start // 86400000 + 1
        indices = np.repeat(np.arange(len(rows)), counts)
        first = np.repeat(np.cumsum(counts) - counts, counts)
        days = start[indices] // 86400000 + np.arange(len(indices)) - first
        columns = {name: pa.array(rows[name][indices]) for name in SCHEMA.names if name in rows.dtype.names}
        columns["start_gpst_ms"] = pa.array(np.maximum(start[indices], days * 86400000))
        columns["end_gpst_ms"] = pa.array(np.minimum(end[indices], (days + 1) * 86400000))
        columns["satellite_system"] = pa.repeat("S", len(indices))
        columns["signal"] = pa.repeat("L1CA", len(indices))
        table = pa.table(columns, schema=SCHEMA)
        for day in np.unique(days):
            self.buffer[int(day) * 86400000].append(table.filter(pa.array(days == day)))
        self.count += len(indices)
        self.total_rows += len(indices)
        if self.count >= 65536:
            self.flush()

    def flush(self):
        for day, tables in self.buffer.items():
            path = self.staging / f"{day}-{len(self.parts[day])}.parquet"
            pq.write_table(pa.concat_tables(tables), path, compression="zstd", compression_level=3)
            self.parts[day].append(path)
        self.buffer.clear()
        self.count = 0

    def finish(self):
        self.flush()
        directory = self.output / "daily"
        directory.mkdir()
        manifest = []
        with tqdm(total=self.total_rows, desc="Write SBAS intervals", unit="row", unit_scale=True, mininterval=1) as progress:
            for day, parts in sorted(self.parts.items()):
                progress.set_postfix_str(label(day // 1000)[:15], refresh=False)
                path = directory / (label(day // 1000)[:15] + ".parquet")
                count = 0
                schema = SCHEMA.with_metadata(
                    {
                        **SCHEMA.metadata,
                        b"day_gpst_ms": str(day).encode(),
                        b"correction_age_seconds": b"600",
                        b"mask_age_seconds": b"1200",
                        b"tail_policy": b"stop at final observed epoch; no extrapolation",
                    }
                )
                with pq.ParquetWriter(path, schema, compression="zstd", compression_level=3) as writer:
                    for part in parts:
                        with pq.ParquetFile(part) as source:
                            for batch in source.iter_batches():
                                writer.write_table(pa.Table.from_batches([batch]).replace_schema_metadata(schema.metadata))
                                count += batch.num_rows
                                progress.update(batch.num_rows)
                manifest.append(dict(path=str(path.relative_to(self.output)), day_gpst_ms=day, rows=count))
                for part in parts:
                    part.unlink()  # Only temporary shards created by this sink.
        self.staging.rmdir()
        return manifest


def build_grid(input_dir, output, gap_timeout=50):
    metadata = json.loads((input_dir / "setup.json").read_text(encoding="utf-8"))
    days = list(day_directories(input_dir))
    if not any(latest_parts(day, "raw-bits") for day in days):
        raise ValueError("No ParquetNEX RawBits inputs in this station")
    inputs = {(day, catalog): latest_parts(day, catalog) for day in days for catalog in ("events", "raw-bits")}
    total_rows = 0
    for paths in inputs.values():
        for path in paths:
            with pq.ParquetFile(path) as source:
                total_rows += source.metadata.num_rows
    sink = DailySink(output)
    processor = _native.GridCnexProcessor(metadata["setup_id"], round(gap_timeout * 1000))

    def batches():
        for day in days:
            day_label = "-".join(day.relative_to(input_dir).parts)
            day_ms = (date.fromisoformat(day_label) - ORIGIN).days * 86400000
            yield "begin", day_label, day_ms, None
            for catalog in ("events", "raw-bits"):
                time_field = "nav_epoch_gpst" if catalog == "raw-bits" else "gpst"
                columns = (
                    ["setup_id", "kind", "scope", "gpst", "payload.epoch_completion.completion"]
                    if catalog == "events"
                    else [
                        "setup_id",
                        "nav_epoch_gpst",
                        "satellite_system",
                        "satellite_number",
                        "message_family",
                        "body_format",
                        "bit_length",
                        "body",
                        "completeness",
                        "checks.list.element.origin",
                        "checks.list.element.kind",
                        "checks.list.element.scope",
                        "checks.list.element.result",
                    ]
                )
                for path in inputs[day, catalog]:
                    with pq.ParquetFile(path) as source:
                        info = source.schema_arrow.metadata or {}
                        if (
                            info.get(b"commonnex.catalog") != catalog.encode()
                            or info.get(b"setup_id") != metadata["setup_id"].encode()
                        ):
                            raise ValueError(f"Unexpected ParquetNEX identity/catalog: {path}")
                        if info.get(b"time.scale") != b"GPST" or source.schema_arrow.field(time_field).type != pa.decimal128(
                            38, 12
                        ):
                            raise ValueError(f"Expected CommonNEX GPST decimal seconds: {path}")
                        for batch in source.iter_batches(batch_size=65536, columns=columns):
                            yield catalog, day_label, path, batch
            yield "end", day_label, None, None

    with tqdm(total=total_rows, desc="SBAS input rows", unit="row", unit_scale=True, mininterval=1) as progress:
        source = prefetched(batches(), thread_name="sbas-read")
        try:
            for catalog, day_label, context, batch in source:
                if catalog == "begin":
                    processor.begin_day(context)
                elif catalog == "end":
                    sink.add(processor.end_day())
                else:
                    progress.set_postfix_str(f"GPST {day_label} {catalog}", refresh=False)
                    try:
                        if catalog == "events":
                            processor.events(batch)
                        else:
                            sink.add(processor.feed(batch))
                    except (ValueError, RuntimeError) as error:
                        raise ValueError(f"{error}: {context}") from error
                    progress.update(batch.num_rows)
            sink.add(processor.finish())
        finally:
            source.close()
    return sink.finish(), processor.diagnostics


@click.command()
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="ParquetNEX station directory containing setup.json and daily RawBits/Events catalogs.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@click.option("--gap-timeout", type=click.FloatRange(min=0.001), default=50, show_default=True)
@staged_output
def cli(input_dir, output, gap_timeout):
    """Compute daily GPST grid intervals from ParquetNEX SBAS RawBits and Events."""
    try:
        output.mkdir()
        manifest, diagnostics = build_grid(input_dir.resolve(), output, gap_timeout)
        result = dict(
            schema=3,
            status="complete",
            product="sbas_igp_intervals",
            time_scale="GPST",
            files=manifest,
            diagnostics=diagnostics,
            policy=dict(
                correction_age_seconds=600,
                mask_age_seconds=1200,
                gap_timeout_seconds=gap_timeout,
                tail="last available navigation context; no cadence extrapolation",
            ),
        )
        write_json(output / "completed.json", result)
        click.echo(json.dumps(dict(status="complete", days=len(manifest), intervals=sum(r["rows"] for r in manifest))))
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
