# SPDX-License-Identifier: GPL-3.0-only
"""Streaming receiver clock telemetry from quality-controlled GPST UBX segments."""

import json
from contextlib import ExitStack
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .protocol import ProtocolWarnings, protocol_option, require_ubx
from .research_output import staged_output
from .sbas_extract import inventory, write_json

SAMPLE_SCHEMA = pa.schema(
    [
        (name, pa.int64())
        for name in (
            "gpst_ns",
            "iTOW_ms",
            "clock_bias_ns",
            "clock_drift_ns_s",
            "time_accuracy_ns",
            "frequency_accuracy_ps_s",
            "clock_adjustment_total_ns",
            "clock_bias_unwrapped_ns",
            "receiver_session_id",
            "clock_arc_id",
            "source_offset",
            "temperature_source_offset",
            "temperature_reference_gpst_ns",
            "runtime_s",
            "timegps_fTOW_ns",
        )
    ]
    + [
        ("temperature_c", pa.float64()),
        ("temperature_age_s", pa.float64()),
        ("rawx_clock_reset", pa.bool_()),
        ("source", pa.string()),
        ("temperature_source", pa.string()),
        ("temperature_association", pa.string()),
        ("unwrap_quality", pa.string()),
        ("time_basis", pa.string()),
    ],
    metadata={b"schema_version": b"1", b"time_scale": b"GPST", b"time_origin": b"1980-01-06 00:00:00 GPST"},
)


class SampleWriter:
    def __init__(self, path, config):
        self.schema = SAMPLE_SCHEMA.with_metadata(
            {
                **SAMPLE_SCHEMA.metadata,
                b"max_gap_seconds": str(config["max_gap"]).encode(),
                b"jump_tolerance_ns": str(config["jump_tolerance_ns"]).encode(),
                b"temperature_max_age_seconds": str(config["temperature_max_age"]).encode(),
            }
        )
        self.writer = pq.ParquetWriter(path, self.schema, compression="zstd")
        self.rows = []

    def add(self, row):
        self.rows.append(row)
        if len(self.rows) >= 65536:
            self.flush()

    def flush(self):
        if self.rows:
            self.writer.write_table(pa.Table.from_pylist(self.rows, schema=self.schema))
            self.rows.clear()

    def close(self):
        self.flush()
        self.writer.close()


def emit_batch(batch, samples, events, telemetry):
    for row in batch["samples"]:
        samples.add(row)
    for key, stream in (("events", events), ("telemetry", telemetry)):
        stream.writelines(json.dumps(row, allow_nan=False) + "\n" for row in batch[key])


def scan(path, tracker, progress, emit, expected=None):
    before = path.stat()
    warnings = ProtocolWarnings(path, "ubx")
    with path.open("rb") as stream:
        while block := stream.read(4 * 1024 * 1024):
            batch = tracker.feed(block, str(path))
            warnings.update(batch)
            emit(batch)
            progress.update(len(block))
    batch = tracker.end_file()
    warnings.update(batch, final=True)
    emit(batch)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or batch["size"] != before.st_size:
        raise ValueError(f"Source changed while reading: {path}")
    if expected and batch["size"] != expected["size"]:
        raise ValueError(f"Reconstruction size mismatch: {path}")
    return {
        "source": str(path),
        **{key: batch[key] for key in ("size", "frames", "skipped_protocol_frames", "skipped_protocol_bytes")},
    }


@click.command()
@protocol_option
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="Completed GPST reconstruction directory; unassigned/ is excluded.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="New output directory.")
@click.option("--max-gap", type=click.FloatRange(min=0, min_open=True), default=50.0, show_default=True)
@click.option("--jump-tolerance-ns", type=click.IntRange(1, 499999), default=50000, show_default=True)
@click.option("--temperature-max-age", type=click.FloatRange(min=0), default=5.0, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, max_gap, jump_tolerance_ns, temperature_max_age, protocol="ubx"):
    """Export NAV-CLOCK, MON-SYS and clock adjustment statistics."""
    try:
        require_ubx(protocol, "NAV-CLOCK/MON-SYS analysis")
        root = input_dir.resolve()
        segments, completed = inventory(root)
        segments.sort(key=lambda s: s["start_gpst"])
        if not segments:
            raise ValueError("No reconstructed GPST segments")
        if any(b["start_gpst"] <= a["end_gpst"] for a, b in zip(segments, segments[1:])):
            raise ValueError("Overlapping reconstructed segments")
        output.mkdir(exist_ok=False)
        config = {"max_gap": max_gap, "jump_tolerance_ns": jump_tolerance_ns, "temperature_max_age": temperature_max_age}
        write_json(
            output / "run.json",
            {
                "schema": 1,
                "time_scale": "GPST",
                "time_origin": "1980-01-06 00:00:00 GPST",
                "config": config,
            },
        )
        with ExitStack() as stack:
            samples = SampleWriter(output / "samples.parquet", config)
            stack.callback(samples.close)
            events = stack.enter_context((output / "events.jsonl").open("x"))
            telemetry = stack.enter_context((output / "mon-sys.jsonl").open("x"))
            sources = stack.enter_context((output / "sources.jsonl").open("x"))

            def emit(stream):
                return lambda row: stream.write(json.dumps(row, allow_nan=False) + "\n")

            tracker = _native.ClockProcessor(
                max_gap=max_gap,
                tolerance=jump_tolerance_ns,
                temperature_max_age=temperature_max_age,
            )
            progress = stack.enter_context(
                tqdm(total=sum(s["size"] for s in segments), desc="Receiver clock", unit="B", unit_scale=True)
            )
            for segment in segments:
                emit(sources)(
                    scan(root / segment["name"], tracker, progress, lambda b: emit_batch(b, samples, events, telemetry), segment)
                )
            emit_batch(tracker.finish(), samples, events, telemetry)
        summary = {
            "status": "complete",
            **tracker.summary(),
        }
        write_json(output / "summary.json", summary)
        click.echo(json.dumps({"status": "complete", **tracker.summary()}))
    except (OSError, ValueError, RuntimeError) as exc:
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
