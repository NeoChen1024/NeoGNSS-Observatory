# SPDX-License-Identifier: GPL-3.0-only
"""Streaming receiver clock telemetry from quality-controlled GPST UBX segments."""

import json
import math
import struct
import subprocess
from collections import Counter
from contextlib import ExitStack
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from .research_output import staged_output
from .sbas_extract import inventory, write_json

WEEK_NS = 604800 * 10**9
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


class ClockTracker:
    """State persists across source files; only missing epochs/restarts break arcs."""

    def __init__(self, sample, event, telemetry, *, max_gap=50.0, tolerance=50000, temperature_max_age=5.0):
        self.sample, self.event, self.telemetry = sample, event, telemetry
        self.max_gap, self.tolerance, self.temperature_max_age = max_gap, tolerance, temperature_max_age
        self.epoch = []
        self.tow = None
        self.uptime = None
        self.temperature = None
        self.session, self.arc, self.adjustment = 0, 0, 0
        self.previous = None
        self.last_reset = None
        self.counts = Counter()
        self.bins = {}
        self.ranges = {}

    def accept(self, source, offset, cls, msg, length, payload):
        key = cls, msg
        sizes = {(1, 0x20): 16, (1, 0x22): 20, (1, 0x61): 4, (10, 0x39): 24}
        if key in sizes and len(payload) != sizes[key]:
            raise ValueError(f"Invalid telemetry payload length: {source}:{offset}")
        tow = None
        if cls == 1:
            tow = struct.unpack_from("<I", payload)[0]
            # Some receivers emit the previous week plus exactly one full
            # week at rollover, while RAWX already uses the following week.
            if tow > 604800000:
                raise ValueError(f"Invalid iTOW={tow}: {source}:{offset} UBX-{cls:02x}-{msg:02x}")
            tow = ((tow + 500) // 1000 * 1000) % 604800000
        elif key == (2, 0x15):
            if len(payload) != 16 or length != 16 + payload[11] * 32 or payload[13] != 1:
                raise ValueError("Invalid or unsupported RAWX payload")
            raw_tow = struct.unpack_from("<d", payload)[0]
            if payload[11]:
                if not math.isfinite(raw_tow) or not 0 <= raw_tow < 604800:
                    raise ValueError("Invalid RAWX rcvTow")
                tow = (math.floor(raw_tow + 0.5) * 1000) % 604800000
        elif key == (10, 0x39) and payload[0] != 1:
            raise ValueError("Unsupported MON-SYS msgVer")
        if tow is not None and self.tow is not None and tow != self.tow:
            self.finish_epoch()
        if tow is not None:
            self.tow = tow
        self.epoch.append((source, offset, cls, msg, payload))
        if len(self.epoch) > 4096:
            raise ValueError("Too many telemetry messages without an epoch boundary")
        if key == (1, 0x61):
            self.finish_epoch()

    def finish_epoch(self):
        if not self.epoch:
            return
        records, self.epoch = self.epoch, []
        tow, self.tow = self.tow, None
        anchors, clocks, monitors = [], [], []
        rawx_reset, ftow = None, None
        basis = None
        for source, offset, cls, msg, p in records:
            if (cls, msg) == (1, 0x20) and p[11] & 3 == 3:
                week = struct.unpack_from("<h", p, 8)[0]
                if week >= 0:
                    raw_itow = struct.unpack_from("<I", p)[0]
                    anchors.append(week * WEEK_NS + ((raw_itow + 500) // 1000) * 10**9)
                    ftow = struct.unpack_from("<i", p, 4)[0]
                    basis = "NAV-TIMEGPS_nominal_iTOW"
            elif (cls, msg) == (2, 0x15):
                if not p[11]:
                    self.counts["empty_rawx_ignored_as_time_anchor"] += 1
                    continue
                seconds, week = struct.unpack_from("<dH", p)
                anchors.append(week * WEEK_NS + math.floor(seconds + 0.5) * 10**9)
                rawx_reset = bool(p[12] & 2) or bool(rawx_reset)
                basis = basis or "RXM-RAWX_nearest_second"
            elif (cls, msg) == (1, 0x22):
                clocks.append((source, offset, p))
            elif (cls, msg) == (10, 0x39):
                monitors.append((source, offset, p))
        if len(set(anchors)) > 1:
            raise ValueError("Conflicting GPST anchors in telemetry epoch")
        gpst = anchors[0] if anchors else None
        if len(clocks) > 1:
            raise ValueError("Multiple NAV-CLOCK messages in one epoch")
        if gpst is not None and clocks:
            # Whole-second anchors establish the week, not the clock sampling
            # cadence. Restore CLOCK's millisecond offset, including week carry.
            itow = struct.unpack_from("<I", clocks[0][2])[0]
            gpst += (itow - ((itow + 500) // 1000) * 1000) * 1000000
            basis = "NAV-CLOCK_iTOW_with_" + basis
        for source, offset, p in monitors:
            runtime = struct.unpack_from("<I", p, 8)[0]
            if self.uptime is not None and runtime < self.uptime:
                self.session += 1
                self.previous = None
                self.temperature = None
                self.last_reset = None
                self.counts["restarts"] += 1
                self.event(
                    {
                        "kind": "receiver_restart",
                        "gpst_ns": gpst,
                        "source": source,
                        "source_offset": offset,
                        "previous_runtime_s": self.uptime,
                        "runtime_s": runtime,
                        "receiver_session_id": self.session,
                    }
                )
            self.uptime = runtime
            self.temperature = {
                "temperature_c": float(struct.unpack_from("<b", p, 18)[0]),
                "temperature_reference_gpst_ns": gpst,
                "temperature_source": source,
                "temperature_source_offset": offset,
            }
            self.telemetry(
                {
                    "kind": "MON-SYS",
                    "association": "stream_epoch_not_measurement_time",
                    "gpst_ns": gpst,
                    "source": source,
                    "source_offset": offset,
                    "runtime_s": runtime,
                    "receiver_session_id": self.session,
                    "payload_hex": p.hex(),
                    **self.temperature,
                }
            )
            self.counts["mon_sys_messages"] += 1
        if not clocks:
            return
        source, offset, p = clocks[0]
        itow, bias, drift, tacc, facc = struct.unpack("<IiiII", p)
        row = dict(
            gpst_ns=gpst,
            iTOW_ms=itow,
            clock_bias_ns=bias,
            clock_drift_ns_s=drift,
            time_accuracy_ns=tacc,
            frequency_accuracy_ps_s=facc,
            source=source,
            source_offset=offset,
            receiver_session_id=self.session,
            runtime_s=self.uptime,
            rawx_clock_reset=rawx_reset,
            timegps_fTOW_ns=ftow,
            time_basis=basis,
            temperature_association="unavailable",
        )
        if self.temperature and gpst is not None and self.temperature["temperature_reference_gpst_ns"] is not None:
            age = (gpst - self.temperature["temperature_reference_gpst_ns"]) / 1e9
            if 0 <= age <= self.temperature_max_age:
                row.update(self.temperature, temperature_age_s=age, temperature_association="stream_epoch_not_measurement_time")
        reason = "arc_start"
        if gpst is None:
            self.previous = None
            self.temperature = None
            row.update(unwrap_quality="unassigned_time")
            self.counts["unassigned_samples"] += 1
        else:
            prev = self.previous
            dt = (gpst - prev["gpst_ns"]) / 1e9 if prev else None
            if prev and dt <= 0:
                raise ValueError(
                    f"Non-increasing NAV-CLOCK GPST: {source}:{offset} "
                    f"gpst_ns={gpst}, iTOW_ms={itow}; previous "
                    f"{prev['source']}:{prev['source_offset']} "
                    f"gpst_ns={prev['gpst_ns']}, iTOW_ms={prev['iTOW_ms']}"
                )
            if prev and dt > self.max_gap:
                reason, prev = "gap", None
                if not monitors:
                    self.temperature = None
                    for key in list(row):
                        if key.startswith("temperature_"):
                            del row[key]
                    row["temperature_association"] = "unavailable"
            if prev:
                jump = bias - prev["clock_bias_ns"] - prev["clock_drift_ns_s"] * dt
                milliseconds = round(jump / 1e6)
                correction = milliseconds * 1000000
                if milliseconds and abs(jump - correction) <= self.tolerance:
                    self.adjustment += correction
                    reason = "rawx_confirmed_adjustment" if rawx_reset else "bias_inferred_adjustment"
                    self.counts[reason] += 1
                    self.event(
                        {
                            "kind": "clock_adjustment",
                            "gpst_ns": gpst,
                            "receiver_session_id": self.session,
                            "clock_arc_id": self.arc,
                            "source": source,
                            "source_offset": offset,
                            "previous_gpst_ns": prev["gpst_ns"],
                            "bias_before_ns": prev["clock_bias_ns"],
                            "bias_after_ns": bias,
                            "drift_ns_s": drift,
                            "jump_residual_ns": jump,
                            "adjustment_ns": correction,
                            "evidence": reason,
                            "temperature_c": row.get("temperature_c"),
                            "interval_since_previous_adjustment_s": (gpst - self.last_reset) / 1e9 if self.last_reset else None,
                        }
                    )
                    self.last_reset = gpst
                elif abs(jump) > self.tolerance or rawx_reset:
                    reason, prev = "unresolved_adjustment", None
                else:
                    reason = "continuous"
            if prev is None:
                self.arc += 1
                self.adjustment = 0
                self.last_reset = None
                self.counts["clock_arcs"] += 1
                self.event(
                    {
                        "kind": "clock_arc_start",
                        "reason": reason,
                        "gpst_ns": gpst,
                        "clock_arc_id": self.arc,
                        "receiver_session_id": self.session,
                    }
                )
            row.update(
                clock_arc_id=self.arc,
                clock_adjustment_total_ns=self.adjustment,
                clock_bias_unwrapped_ns=bias - self.adjustment,
                unwrap_quality=reason,
            )
            self.previous = row
        self.sample(row)
        self.counts["samples"] += 1
        self.counts["samples_with_temperature" if row.get("temperature_c") is not None else "samples_without_temperature"] += 1
        for field in ("gpst_ns", "clock_bias_ns", "clock_drift_ns_s", "temperature_c"):
            value = row.get(field)
            if value is not None:
                low, high = self.ranges.get(field, (value, value))
                self.ranges[field] = min(low, value), max(high, value)
        if row.get("temperature_c") is not None:
            # Integer-degree bins, online moments; no full-run sample retention.
            key = int(row["temperature_c"])
            n, mean, m2 = self.bins.get(key, (0, 0.0, 0.0))
            n += 1
            delta = drift - mean
            mean += delta / n
            m2 += delta * (drift - mean)
            self.bins[key] = n, mean, m2

    def summary(self):
        n = sum(v[0] for v in self.bins.values())
        correlation = None
        if n:
            mt = sum(t * v[0] for t, v in self.bins.items()) / n
            md = sum(v[0] * v[1] for v in self.bins.values()) / n
            vt = sum(v[0] * (t - mt) ** 2 for t, v in self.bins.items())
            vd = sum(m2 + count * (mean - md) ** 2 for count, mean, m2 in self.bins.values())
            cov = sum(count * (t - mt) * (mean - md) for t, (count, mean, _) in self.bins.items())
            if vt > 0 and vd > 0:
                correlation = max(-1.0, min(1.0, cov / math.sqrt(vt * vd)))
        return {
            "counts": dict(self.counts),
            "receiver_sessions": self.session + 1 if self.counts else 0,
            "ranges": {k: {"min": lo, "max": hi} for k, (lo, hi) in self.ranges.items()},
            "temperature_drift_pearson_r": correlation,
            "temperature_drift_bins": [
                {"temperature_c": t, "samples": n, "mean_drift_ns_s": mean, "stddev_drift_ns_s": math.sqrt(m2 / n)}
                for t, (n, mean, m2) in sorted(self.bins.items())
            ],
        }


def scan(path, worker, tracker, progress, expected=None):
    before = path.stat()
    summary = None
    consumed = 0
    with subprocess.Popen([str(worker), str(path)], stdout=subprocess.PIPE, text=True) as process:
        try:
            for line in process.stdout:
                fields = line.rstrip("\n").split("\t")
                if fields[0] == "summary":
                    summary = {"size": int(fields[1]), "frames": int(fields[2])}
                    continue
                offset, cls, msg, length = map(int, fields[:4])
                tracker.accept(str(path), offset, cls, msg, length, bytes.fromhex(fields[4]))
                progress.update(offset - consumed)
                consumed = offset
            if process.wait() or summary is None:
                raise ValueError(f"Native clock scan failed: {path}")
        except BaseException:
            process.terminate()
            raise
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or summary["size"] != before.st_size:
        raise ValueError(f"Source changed while reading: {path}")
    if expected and summary["size"] != expected["size"]:
        raise ValueError(f"Reconstruction checksum mismatch: {path}")
    progress.update(summary["size"] - consumed)
    return {"source": str(path), **summary}


@click.command()
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="Completed GPST reconstruction directory; unassigned/ is excluded.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="New output directory.")
@click.option("--worker", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--max-gap", type=click.FloatRange(min=0, min_open=True), default=50.0, show_default=True)
@click.option("--jump-tolerance-ns", type=click.IntRange(1, 499999), default=50000, show_default=True)
@click.option("--temperature-max-age", type=click.FloatRange(min=0), default=5.0, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, worker, max_gap, jump_tolerance_ns, temperature_max_age):
    """Export NAV-CLOCK, MON-SYS and clock adjustment statistics."""
    try:
        root, worker = input_dir.resolve(), worker.resolve()
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

            tracker = ClockTracker(
                samples.add,
                emit(events),
                emit(telemetry),
                max_gap=max_gap,
                tolerance=jump_tolerance_ns,
                temperature_max_age=temperature_max_age,
            )
            progress = stack.enter_context(
                tqdm(total=sum(s["size"] for s in segments), desc="Receiver clock", unit="B", unit_scale=True)
            )
            for segment in segments:
                emit(sources)(scan(root / segment["name"], worker, tracker, progress, segment))
            tracker.finish_epoch()
        summary = {
            "status": "complete",
            **tracker.summary(),
        }
        write_json(output / "summary.json", summary)
        click.echo(json.dumps({"status": "complete", **tracker.summary()}))
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
