# SPDX-License-Identifier: GPL-3.0-only
"""Produce daily GPST grid intervals from protocol-neutral SBAS streams."""

import json
from collections import Counter, defaultdict
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .gpst import label
from .research_output import staged_output, write_json
from .sbas_frames import SCHEMA as FRAME_SCHEMA

SCHEMA = pa.schema(
    [
        (name, pa.int64())
        for name in (
            "start_gpst_ms",
            "end_gpst_ms",
            "prn",
            "band",
            "mask_bit",
            "iodi",
            "givei",
            "frame_id",
            "stream_id",
        )
    ]
    + [(name, pa.float64()) for name in ("latitude", "longitude", "delay_m", "vtec_tecu")]
    + [(name, pa.string()) for name in ("constellation", "signal")],
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
        self.parts = defaultdict(list)

    def add(self, row):
        start, end = row["start_gpst_ms"], row["end_gpst_ms"]
        while start < end:
            day = start // 86400000 * 86400000
            stop = min(end, day + 86400000)
            self.buffer[day].append(dict(row, start_gpst_ms=start, end_gpst_ms=stop))
            self.count += 1
            start = stop
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
        directory = self.output / "daily"
        directory.mkdir()
        manifest = []
        for day, parts in tqdm(sorted(self.parts.items()), desc="Write GPST days", unit="day"):
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
                    for batch in pq.ParquetFile(part).iter_batches():
                        writer.write_table(pa.Table.from_batches([batch]).replace_schema_metadata(schema.metadata))
                        count += batch.num_rows
            manifest.append(dict(path=str(path.relative_to(self.output)), day_gpst_ms=day, rows=count))
            for part in parts:
                part.unlink()  # Only temporary shards created by this sink.
        self.staging.rmdir()
        return manifest


def build_grid(input_dir, output):
    daily = input_dir / "daily" if (input_dir / "daily").is_dir() else input_dir
    paths = sorted(daily.glob("GPST-*.parquet"))
    if not paths:
        raise ValueError("No daily SBAS frame Parquet inputs")
    sink = DailySink(output)
    states, identities, pending = {}, {}, {}
    ended = set()
    diagnostics = Counter()

    def emit(key, rows):
        for row in rows:
            frame_id = row.pop("frame_offset")
            sink.add(dict(row, **identities[key], stream_id=key, frame_id=frame_id))

    def flush(key):
        if pending[key]:
            emit(key, states[key].process_frames(pending[key]))
            pending[key].clear()

    for path in tqdm(paths, desc="SBAS frame days", unit="day"):
        with pq.ParquetFile(path) as source:
            schema = source.schema_arrow
            if not schema.equals(FRAME_SCHEMA) or any(
                (schema.metadata or {}).get(k) != v for k, v in FRAME_SCHEMA.metadata.items()
            ):
                raise ValueError(f"Unexpected SBAS frame schema: {path}")
            day = int(schema.metadata[b"day_gpst_ms"])
            if day % 86400000:
                raise ValueError("Frame partition is not a GPST day")
            for batch in source.iter_batches(batch_size=8192):
                for row in batch.to_pylist():
                    if not day <= row["gpst_ms"] < day + 86400000:
                        raise ValueError("SBAS record outside its GPST day")
                    key = row["stream_id"]
                    identity = {k: row[k] for k in ("constellation", "prn", "signal")}
                    if identity["constellation"] != "SBAS" or identity["signal"] != "L1CA":
                        raise ValueError("Grid requires SBAS L1CA")
                    if key in ended:
                        raise ValueError("SBAS stream continued after its end marker")
                    if key not in states:
                        if row["kind"] != "frame":
                            raise ValueError("SBAS stream has no initial frame")
                        states[key], pending[key], identities[key] = _native.GridProcessor(), [], identity
                    elif identities[key] != identity:
                        raise ValueError("SBAS stream identity changed")
                    if row["kind"] == "frame":
                        if row["frame_id"] is None or row["frame"] is None or row["crc_valid"] is None:
                            raise ValueError("Incomplete SBAS frame record")
                        pending[key].append({k: row[k] for k in ("gpst_ms", "frame_id", "frame", "crc_valid", "accepted")})
                    elif row["kind"] == "end":
                        flush(key)
                        emit(key, states[key].finish(row["gpst_ms"]))
                        diagnostics.update(states[key].diagnostics)
                        del states[key], pending[key], identities[key]
                        ended.add(key)
                    else:
                        raise ValueError("Unknown SBAS record kind")
                for key in states:
                    flush(key)
    if states:
        raise ValueError("Incomplete SBAS streams: missing end markers")
    return sink.finish(), dict(diagnostics)


@click.command()
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="Daily SBAS frame Parquet; no raw recordings or reconstruction indexes required.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output):
    """Compute daily GPST grid intervals from source-independent SBAS frames."""
    try:
        output.mkdir()
        manifest, diagnostics = build_grid(input_dir.resolve(), output)
        result = dict(
            schema=3,
            status="complete",
            product="sbas_igp_intervals",
            time_scale="GPST",
            files=manifest,
            diagnostics=diagnostics,
            policy=dict(correction_age_seconds=600, mask_age_seconds=1200, tail="explicit stream end; no cadence extrapolation"),
        )
        write_json(output / "completed.json", result)
        click.echo(json.dumps(dict(status="complete", days=len(manifest), intervals=sum(r["rows"] for r in manifest))))
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
