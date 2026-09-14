# SPDX-License-Identifier: GPL-3.0-only
"""Produce daily GPST grid intervals from protocol-neutral SBAS streams."""

import json
import re
from collections import Counter, defaultdict
from datetime import date
from decimal import Decimal
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .cnex_import import ORIGIN, latest_parts
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


def build_grid(input_dir, output, gap_timeout=50):
    metadata = json.loads((input_dir / "stream.json").read_text(encoding="utf-8"))
    days = sorted(p for p in input_dir.iterdir() if p.is_dir() and re.fullmatch(r"\d{4}-\d{2}-\d{2}", p.name))
    if not any(latest_parts(day, "raw-bits") for day in days):
        raise ValueError("No ParquetNEX RawBits inputs in this Stream")
    sink = DailySink(output)
    states, identities, pending = {}, {}, {}
    last_frame = {}
    next_stream = next_frame = 0
    previous_nav = previous_raw = None
    gap_ms = round(gap_timeout * 1000)
    diagnostics = Counter()

    def emit(key, rows):
        for row in rows:
            frame_id = row.pop("frame_offset")
            sink.add(dict(row, **identities[key], frame_id=frame_id))

    def flush(key):
        if pending[key]:
            emit(key, states[key].process_frames(pending[key]))
            pending[key].clear()

    def close(key, time):
        flush(key)
        emit(key, states[key].finish(time))
        diagnostics.update(states[key].diagnostics)
        del states[key], pending[key], identities[key], last_frame[key]

    def advance(time):
        nonlocal previous_nav
        if previous_nav is not None:
            if time < previous_nav:
                raise ValueError("Reversed navigation context; select a non-overlapping recording path")
            if time - previous_nav > gap_ms:
                for key in list(states):
                    close(key, max(previous_nav, last_frame[key]))
                diagnostics["navigation_gaps"] += 1
        previous_nav = time
        for key in list(states):
            if time - last_frame[key] > gap_ms:
                close(key, last_frame[key])
                diagnostics["signal_gaps"] += 1

    def rows(day, catalog):
        start = Decimal((date.fromisoformat(day.name) - ORIGIN).days * 86400)
        time_field = "nav_epoch_gpst" if catalog == "raw-bits" else "gpst"
        for path in latest_parts(day, catalog):
            with pq.ParquetFile(path) as source:
                info = source.schema_arrow.metadata or {}
                if info.get(b"commonnex.catalog") != catalog.encode() or info.get(b"stream_id") != metadata["stream_id"].encode():
                    raise ValueError(f"Unexpected ParquetNEX identity/catalog: {path}")
                if info.get(b"time.scale") != b"GPST" or source.schema_arrow.field(time_field).type != pa.decimal128(38, 12):
                    raise ValueError(f"Expected CommonNEX GPST decimal seconds: {path}")
                for batch in source.iter_batches(batch_size=8192):
                    if catalog == "events":
                        batch = batch.filter(pc.equal(batch.column("scope"), "NAVIGATION"))
                    else:
                        batch = batch.filter(pc.equal(batch.column("message_family"), "SBAS_L1"))
                    for row in batch.to_pylist():
                        if row["stream_id"] != metadata["stream_id"]:
                            raise ValueError(f"Mixed Stream identity: {path}")
                        if not start <= row[time_field] < start + 86400:
                            raise ValueError(f"Record outside its GPST day: {path}")
                        yield row

    for day in tqdm(days, desc="ParquetNEX SBAS days", unit="day"):
        # Completion context is independent of measurement timestamps. Read one
        # day's small navigation event catalog, not all observations or RawBits.
        times = []
        for event in rows(day, "events"):
            if event["kind"] == "EPOCH_COMPLETION":
                if event["payload"]["epoch_completion"]["completion"] != "COMPLETE":
                    raise ValueError("Incomplete navigation closure requires an explicit downstream policy")
                times.append(int(event["gpst"] * Decimal(1000)))
            else:
                raise ValueError(f"Unsupported navigation event: {event['kind']}")
        times = iter(sorted(set(times)))
        context = next(times, None)
        for row in rows(day, "raw-bits"):
            time = int(row["nav_epoch_gpst"] * Decimal(1000))
            if previous_raw is not None and time < previous_raw:
                raise ValueError("Reversed SBAS occurrence time; select non-overlapping inputs")
            previous_raw = time
            while context is not None and context <= time:
                advance(context)
                context = next(times, None)
            if row["satellite_system"] != "S" or row["body_format"] != "SBAS_L1_250_V1" or row["bit_length"] != 250:
                raise ValueError("Grid requires canonical SBAS L1 250-bit bodies")
            if row["completeness"] != "complete" or len(row["body"]) != 32 or row["body"][-1] & 63:
                raise ValueError("Invalid complete SBAS body")
            key = row["satellite_number"]
            if key in states and time - last_frame[key] > gap_ms:
                close(key, last_frame[key])
                diagnostics["signal_gaps"] += 1
            if key not in states:
                states[key], pending[key] = _native.GridProcessor(), []
                identities[key] = dict(satellite_system="S", satellite_number=key, signal="L1CA", stream_id=next_stream)
                next_stream += 1
            checks = {(c["origin"], c["kind"], c["scope"]): c["result"] for c in row["checks"]}
            independent = checks.get(("independent", "crc", "message"))
            if independent not in ("pass", "fail"):
                raise ValueError("Missing independently checked SBAS CRC")
            receiver = checks.get(("receiver", "crc", "message"))
            pending[key].append(
                dict(
                    gpst_ms=time,
                    frame_id=next_frame,
                    frame=row["body"],
                    crc_valid=independent == "pass",
                    accepted=receiver == "pass" if receiver in ("pass", "fail") else None,
                )
            )
            next_frame += 1
            last_frame[key] = time
            if len(pending[key]) >= 8192:
                flush(key)
        while context is not None:
            advance(context)
            context = next(times, None)
        for key in states:
            flush(key)
    for key in list(states):
        close(key, max(last_frame[key], previous_nav if previous_nav is not None else last_frame[key]))
    diagnostics["raw_bits_frames"] = next_frame
    return sink.finish(), dict(diagnostics)


@click.command()
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="ParquetNEX Stream directory containing stream.json and daily RawBits/Events catalogs.",
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
