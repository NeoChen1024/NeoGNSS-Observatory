# SPDX-License-Identifier: GPL-3.0-only
"""Streaming UBX/SBF receiver clock and independent PPS telemetry."""

import json
from contextlib import ExitStack
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .dataset_inputs import recordings
from .protocol import ProtocolWarnings, protocol_option
from .research_output import staged_output, write_json

CLOCK_SCHEMA = pa.schema(
    [
        (name, pa.int64())
        for name in (
            "gpst_ns",
            "iTOW_ms",
            "time_accuracy_ns",
            "frequency_accuracy_ps_s",
            "clock_adjustment_total_ns",
            "receiver_session_id",
            "clock_arc_id",
            "source_offset",
            "source_id",
            "adjustment_ns",
            "timegps_fTOW_ns",
            "cumulative_clock_jumps_ms",
        )
    ]
    + [
        ("clock_bias_ns", pa.float64()),
        ("clock_drift_ns_s", pa.float64()),
        ("clock_bias_unwrapped_ns", pa.float64()),
        ("rawx_clock_reset", pa.bool_()),
        ("adjustment_evidence", pa.string()),
        ("arc_start_reason", pa.string()),
        ("time_basis", pa.string()),
    ],
    metadata={b"schema_version": b"3", b"time_scale": b"GPST", b"time_origin": b"1980-01-06 00:00:00 GPST"},
)

STATUS_SCHEMA = pa.schema(
    [(k, pa.int64()) for k in ("gpst_ns", "runtime_s", "receiver_session_id", "source_id", "source_offset")]
    + [("temperature_c", pa.float64()), ("restart", pa.bool_()), ("fine_time", pa.bool_())]
    + [(k, pa.string()) for k in ("message", "time_basis")],
    metadata=CLOCK_SCHEMA.metadata,
)

PPS_SCHEMA = pa.schema(
    [
        (k, pa.int64())
        for k in (
            "gpst_ns",
            "reference_id",
            "utc_standard",
            "native_week",
            "native_tow_ms",
            "native_tow_sub_ms",
            "raim",
            "source_offset",
            "source_id",
        )
    ]
    + [("quantization_error_ns", pa.float64()), ("quantization_error_valid", pa.bool_()), ("sync_age_s", pa.float64())]
    + [(k, pa.string()) for k in ("message", "time_association", "reference_time_scale")],
    metadata={
        b"time_scale": b"GPST",
        b"time_origin": b"1980-01-06 00:00:00 GPST",
        b"error_units": b"ns",
        b"error_sign": b"native protocol convention",
    },
)


class SampleWriter:
    def __init__(self, path, config, schema=CLOCK_SCHEMA):
        self.schema = schema.with_metadata(
            {
                **schema.metadata,
                b"protocol": config["protocol"].encode(),
                b"max_gap_seconds": str(config["max_gap"]).encode(),
                b"jump_tolerance_ns": str(config["jump_tolerance_ns"]).encode(),
                b"temperature_max_age_seconds": str(config["temperature_max_age"]).encode(),
            }
        )
        self.writer = pq.ParquetWriter(path, self.schema, compression="zstd", compression_level=3)
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


def emit_batch(batch, clock, status, pps, source_ids):
    def identify(row):
        row = dict(row)
        row["source_id"] = source_ids[row["source"]]
        return row

    changes = {}
    restarts = set()
    for event in batch["events"]:
        if event["kind"] == "receiver_restart":
            restarts.add((event["receiver_session_id"], event["runtime_s"]))
        else:
            key = (event.get("gpst_ns"), event["receiver_session_id"], event["clock_arc_id"])
            change = changes.setdefault(key, {})
            if event["kind"] == "clock_adjustment":
                change.update(adjustment_ns=event["adjustment_ns"], adjustment_evidence=event["evidence"])
            else:
                change["arc_start_reason"] = event["reason"]
    for row in batch["samples"]:
        row = identify(row)
        row["adjustment_ns"] = 0
        row.update(changes.get((row.get("gpst_ns"), row["receiver_session_id"], row.get("clock_arc_id")), {}))
        clock.add(row)
    for row in batch["telemetry"]:
        row = identify(row)
        row["message"] = row["kind"]
        row["time_basis"] = row.get("association", "SBF_WNc_TOW")
        row["restart"] = (row["receiver_session_id"], row["runtime_s"]) in restarts
        restarts.discard((row["receiver_session_id"], row["runtime_s"]))
        status.add(row)
    for row in batch["pps"]:
        pps.add(identify(row))


def scan(path, tracker, progress, emit, initial, protocol):
    warnings = ProtocolWarnings(path, protocol, initial)
    size = 0
    batch = initial
    with path.open("rb") as stream:
        while block := stream.read(4 * 1024 * 1024):
            batch = tracker.feed(block, str(path))
            warnings.update(batch)
            emit(batch)
            size += len(block)
            progress.update(len(block))
    warnings.update(batch, final=True)
    return dict(source=str(path), size=size), batch


@click.command()
@protocol_option
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="Expanded UBX/SBF recordings in stream order; unassigned/ is excluded.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="New output directory.")
@click.option("--max-gap", type=click.FloatRange(min=0, min_open=True), default=50.0, show_default=True)
@click.option("--jump-tolerance-ns", type=click.IntRange(1, 499999), default=50000, show_default=True)
@click.option("--temperature-max-age", type=click.FloatRange(min=0), default=5.0, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, max_gap, jump_tolerance_ns, temperature_max_age, protocol="ubx"):
    """Export receiver clock, temperature, uptime and PPS telemetry."""
    try:
        root = input_dir.resolve()
        paths = recordings(root, protocol)
        output.mkdir(exist_ok=False)
        config = {
            "max_gap": max_gap,
            "jump_tolerance_ns": jump_tolerance_ns,
            "temperature_max_age": temperature_max_age,
            "protocol": protocol,
        }
        source_ids = {str(path): i for i, path in enumerate(paths)}
        sources = []
        with ExitStack() as stack:
            clock = SampleWriter(output / "clock.parquet", config)
            stack.callback(clock.close)
            status = SampleWriter(output / "status.parquet", config, STATUS_SCHEMA)
            stack.callback(status.close)
            pps = SampleWriter(output / "pps.parquet", config, PPS_SCHEMA)
            stack.callback(pps.close)
            tracker = _native.ClockProcessor(
                max_gap=max_gap,
                tolerance=jump_tolerance_ns,
                temperature_max_age=temperature_max_age,
                protocol=protocol,
            )
            progress = stack.enter_context(
                tqdm(total=sum(p.stat().st_size for p in paths), desc="Receiver clock", unit="B", unit_scale=True)
            )
            counters = {}
            for path in paths:
                record, counters = scan(
                    path, tracker, progress, lambda b: emit_batch(b, clock, status, pps, source_ids), counters, protocol
                )
                sources.append(dict(source_id=source_ids[str(path)], **record))
            emit_batch(tracker.finish(), clock, status, pps, source_ids)
        summary = {
            "status": "complete",
            "schema": 3,
            "time_scale": "GPST",
            "time_origin": "1980-01-06 00:00:00 GPST",
            "config": config,
            "sources": sources,
            **tracker.summary(),
        }
        write_json(output / "summary.json", summary)
        click.echo(json.dumps({"status": "complete", **tracker.summary()}))
    except (OSError, ValueError, RuntimeError) as exc:
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
