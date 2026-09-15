# SPDX-License-Identifier: GPL-3.0-only
"""Vectorized clock analysis shared by CommonNEX processing and re-unwrapping."""

import json
import shutil
from collections import Counter
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from .research_output import staged_output, write_json


class BatchUnwrapper:
    """Vectorized Parquet reduction using the native clock processor's adjustment/timeout policy."""

    def __init__(self, max_gap=50.0, tolerance=50000):
        self.max_gap, self.tolerance = max_gap, tolerance
        self.previous = None
        self.arc = 0
        self.adjustment = 0
        self.counts = Counter()

    def apply(self, table):
        def integer(name, missing=0):
            return table[name].combine_chunks().fill_null(missing).to_numpy(zero_copy_only=False)

        t = integer("gpst_ns", -1)
        if not len(t):
            return table
        bias, drift = (integer(n, float("nan")) for n in ("clock_bias_ns", "clock_drift_ns_s"))
        session = integer("receiver_session_id")
        flag = integer("rawx_clock_reset", False)
        cumulative = (
            integer("cumulative_clock_jumps_ms", 256) if "cumulative_clock_jumps_ms" in table.column_names else np.full(len(t), 256)
        )
        sbf = (
            np.array([s in ("SBF-PVTGeodetic", "SBF-PVTCartesian") for s in table["source_message"].to_pylist()])
            if "source_message" in table.column_names
            else np.zeros(len(t), bool)
        )
        valid = (t >= 0) & np.isfinite(bias) & np.isfinite(drift)
        if "continuity_known" in table.column_names:
            valid &= integer("continuity_known", False)
        current = (t, bias, drift, session, cumulative, sbf, valid)
        before = self.previous or (-1, 0, 0, int(session[0]), 256, False, False)
        pt, pb, pd, ps, pc, prev_sbf, prior_valid = (np.r_[value, values[:-1]] for value, values in zip(before, current))
        restart = session != ps
        dt = (t - pt) / 1e9
        if np.any(valid & prior_valid & ~restart & (dt <= 0)):
            raise ValueError("Non-increasing GPST in clock samples")
        gap = valid & prior_valid & ~restart & (dt > self.max_gap)
        fresh = valid & (~prior_valid | restart | gap)
        comparable = valid & ~fresh
        jump = bias - pb - pd * dt
        milliseconds = np.rint(np.where(comparable, jump, 0) / 1e6)
        if np.any(np.abs(milliseconds) > np.iinfo(np.int64).max // 1000000):
            raise ValueError("Clock adjustment exceeds int64 range")
        rounded = milliseconds.astype(np.int64) * 1000000
        counted = comparable & sbf & prev_sbf & (cumulative < 256) & (pc < 256)
        # The adapter-specific modulo is not inferred from uint64 storage.
        current_counter = np.where(cumulative < 256, cumulative, 256).astype(np.int64)
        previous_counter = np.where(pc < 256, pc, 256).astype(np.int64)
        delta = (current_counter - previous_counter + 128) % 256 - 128
        rounded = np.where(counted, delta * 1000000, rounded)
        adjusted = comparable & (rounded != 0) & (np.abs(jump - rounded) <= self.tolerance)
        unresolved = comparable & ~adjusted & ((np.abs(jump) > self.tolerance) | flag | (counted & (delta != 0)))
        fresh |= unresolved
        arc = self.arc + np.cumsum(fresh)
        total = self.adjustment + np.cumsum(np.where(adjusted, rounded, 0))
        # Reset only at a genuine new arc, never at an old extractor arc or
        # an input file, Parquet row group, batch, hour, or plot boundary.
        reset_at = np.maximum.accumulate(np.where(fresh, np.arange(len(t)) + 1, 0))
        total -= np.r_[0, total][reset_at]
        quality = np.full(len(t), "continuous", dtype=object)
        quality[~valid] = "unassigned_time"
        quality[fresh] = "arc_start"
        quality[gap] = "gap"
        quality[unresolved] = "unresolved_adjustment"
        quality[adjusted & ~flag] = "bias_inferred_adjustment"
        quality[adjusted & flag] = "rawx_confirmed_adjustment"
        quality[adjusted & counted] = "sbf_counted_adjustment"
        self.previous = tuple(values[-1].item() for values in current)
        self.arc, self.adjustment = int(arc[-1]), int(total[-1])
        self.counts.update(
            {
                "samples": len(t),
                "clock_arcs": int(np.sum(fresh)),
                "unassigned_samples": int(np.sum(t < 0)),
                "unusable_samples": int(np.sum(~valid)),
                "bias_inferred_adjustment": int(np.sum(adjusted & ~flag & ~counted)),
                "rawx_confirmed_adjustment": int(np.sum(adjusted & flag & ~counted)),
                "sbf_counted_adjustment": int(np.sum(adjusted & counted)),
            }
        )
        for name, values in (
            ("clock_arc_id", arc),
            ("clock_adjustment_total_ns", total),
            ("clock_bias_unwrapped_ns", bias - total),
            ("adjustment_ns", np.where(adjusted, rounded, 0)),
        ):
            dtype = pa.float64() if name == "clock_bias_unwrapped_ns" else pa.int64()
            table = table.set_column(table.schema.get_field_index(name), name, pa.array(values, mask=~valid, type=dtype))
        for name, mask in (("adjustment_evidence", adjusted), ("arc_start_reason", fresh)):
            table = table.set_column(table.schema.get_field_index(name), name, pa.array(quality, mask=~mask, type=pa.string()))
        return table


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option(
    "--output", type=click.Path(path_type=Path), required=True, help="New telemetry directory; raw sample columns are preserved."
)
@click.option("--max-gap", type=click.FloatRange(min=0, min_open=True), default=50.0, show_default=True)
@click.option("--jump-tolerance-ns", type=click.IntRange(1, 499999), default=50000, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, max_gap, jump_tolerance_ns):
    """Recalculate unwrapped bias from existing receiver-clock Parquet."""
    try:
        source, output = input_dir.resolve(), output.resolve()
        summary = json.loads((source / "summary.json").read_text())
        parquet = pq.ParquetFile(source / "clock.parquet")
        if parquet.schema_arrow.metadata.get(b"input_format") != b"CommonNEX":
            raise ValueError("Re-import receiver-clock analysis from CommonNEX before re-unwrapping")
        if parquet.schema_arrow.metadata.get(b"time_scale") != b"GPST":
            raise ValueError("Expected GPST sample timestamps")
        output.mkdir(parents=True, exist_ok=False)
        for name in ("status.parquet", "pps.parquet"):
            shutil.copyfile(source / name, output / name)
        tracker = BatchUnwrapper(max_gap, jump_tolerance_ns)
        metadata = {
            **parquet.schema_arrow.metadata,
            b"max_gap_seconds": str(max_gap).encode(),
            b"jump_tolerance_ns": str(jump_tolerance_ns).encode(),
        }
        with pq.ParquetWriter(
            output / "clock.parquet", parquet.schema_arrow.with_metadata(metadata), compression="zstd", compression_level=3
        ) as writer:
            with tqdm(total=parquet.metadata.num_rows, desc="Reunwrap clock", unit="sample", unit_scale=True) as progress:
                for batch in parquet.iter_batches(batch_size=65536):
                    writer.write_table(tracker.apply(pa.Table.from_batches([batch])).replace_schema_metadata(metadata))
                    progress.update(batch.num_rows)
        summary["config"].update(max_gap=max_gap, jump_tolerance_ns=jump_tolerance_ns)
        summary["operation"] = "reunwrap_from_clock_table"
        summary.get("ranges", {}).pop("clock_bias_unwrapped_ns", None)
        summary["counts"].update(tracker.counts)
        write_json(output / "summary.json", summary)
        click.echo(json.dumps({"status": "complete"}))
    except (OSError, ValueError, KeyError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
