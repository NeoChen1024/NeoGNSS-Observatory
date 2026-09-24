# SPDX-License-Identifier: GPL-3.0-only
"""Receiver-clock analysis from CommonNEX, without protocol decoding."""

import json
from collections import Counter
from contextlib import ExitStack
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from tqdm import tqdm

from .clock_status import StatusJoin
from .cnex_import import day_directories, dictionary_columns, latest_parts
from .receiver_clock_reunwrap import BatchUnwrapper
from .research_output import staged_output, write_json
from .setup_metadata import validate_setup

TIME = pa.decimal128(38, 12)
METADATA = {b"time_scale": b"GPST", b"time_origin": b"1980-01-06 00:00:00 GPST"}


def ns(column):
    """Decimal -> signed integer ns, truncating <1 ns only in derived columns.

    Arrow decimal buffers hold signed 128-bit unscaled integers. Avoid forming
    large absolute binary64 timestamps; canonical decimal columns are retained.
    """
    a = pc.cast(column, pa.decimal128(38, 9), safe=False).combine_chunks()
    valid = pc.is_valid(a).to_numpy(zero_copy_only=False)
    if not len(a):
        return np.array([], dtype=np.int64), valid
    words = np.frombuffer(a.buffers()[1], dtype="<i8").reshape(-1, 2)[a.offset : a.offset + len(a)]
    low, high = words[:, 0], words[:, 1]
    if np.any(valid & (high != np.where(low < 0, -1, 0))):
        raise ValueError("Derived nanosecond timestamp exceeds int64")
    return np.where(valid, low, -1), valid


def numeric(table, name):
    return pc.cast(table[name], pa.float64()).to_numpy(zero_copy_only=False)


def nanoseconds(table, name):
    # Scale in decimal before the final float conversion: integer-ns source
    # values remain exactly representable and sub-ns reports are not truncated.
    wide = pc.cast(table[name], pa.decimal256(38, 12))
    return pc.cast(pc.multiply(wide, pa.scalar(10**9, pa.int64())), pa.float64()).to_numpy(zero_copy_only=False)


def catalog(day, name, setup_id, columns=None):
    parts = latest_parts(day, name)
    tables = []
    for path in parts:
        with pq.ParquetFile(path) as source:
            schema = source.schema_arrow
            meta = schema.metadata or {}
            if meta.get(b"commonnex.catalog") != name.encode() or meta.get(b"setup_id") != setup_id.encode():
                raise ValueError(f"Unexpected CommonNEX catalog/Setup: {path}")
            key = "gpst"
            if meta.get(b"time.scale") != b"GPST" or schema.field(key).type != TIME:
                raise ValueError(f"Expected CommonNEX GPST decimal time: {path}")
            table = source.read(columns=columns)
            if not pc.all(pc.fill_null(pc.equal(table["setup_id"], setup_id), False)).as_py() and len(table):
                raise ValueError(f"Mixed Setup identity: {path}")
            tables.append(table)
    return pa.concat_tables(tables) if tables else None


def telemetry_status(table):
    if table is None:
        return None
    selected = pc.or_(pc.is_valid(table["receiver_uptime_s"]), pc.is_valid(table["receiver_temperature_c"]))
    return table.select(STATUS_COLUMNS).filter(selected)


STATUS_COLUMNS = ["setup_id", "gpst", "receiver_uptime_s", "receiver_temperature_c", "cpu_load_percent"]


def telemetry_pulses(table):
    if table is None:
        return None
    values = pc.list_flatten(table["pulse_timing"]).combine_chunks()
    return pa.Table.from_arrays(
        [values.field(f.name) for f in values.type],
        names=["pulse_quantization_error_s" if f.name == "quantization_error_s" else f.name for f in values.type],
    )


def measurement_evidence(column):
    # Reduce only for the unwrap algorithm; the complete ordered source list
    # remains on the derived clock rows. Any explicit reset flags the cycle;
    # the last reported counter describes its final reported adjustment state.
    column = column.combine_chunks()
    parents = pc.list_parent_indices(column).to_numpy(zero_copy_only=False)
    values = pc.list_flatten(column)
    reported = values.field("adjustment_reported")
    valid = pc.is_valid(reported).to_numpy(zero_copy_only=False)
    flags = np.zeros(len(column), dtype=bool)
    present = np.zeros(len(column), dtype=bool)
    np.logical_or.at(flags, parents, reported.fill_null(False).to_numpy(zero_copy_only=False))
    present[parents[valid]] = True
    cumulative = values.field("cumulative_adjustment_ms")
    valid = pc.is_valid(cumulative).to_numpy(zero_copy_only=False)
    last = np.full(len(column), -1, dtype=np.int64)
    np.maximum.at(last, parents[valid], np.flatnonzero(valid))
    counters = pc.take(cumulative, pa.array(last, mask=last < 0))
    moduli = pc.take(values.field("cumulative_adjustment_modulus_ms"), pa.array(last, mask=last < 0))
    return pa.array(flags, mask=~present), counters, moduli


class Sessions:
    """Compact restart boundaries; never guess GPST from receiver uptime."""

    def __init__(self, days, setup_id):
        self.starts = []
        self.uncertain = []
        self.status_changes = {}
        session, last_time, pending = 0, -1, None
        for day in tqdm(days, desc="Receiver restart context", unit="day"):
            status = telemetry_status(catalog(day, "receiver-telemetry", setup_id, STATUS_COLUMNS))
            events = catalog(day, "events", setup_id, ["setup_id", "gpst", "kind", "receiver_uptime_s"])
            changes = []
            if events is not None:
                events = events.filter(pc.equal(events["kind"], "RECEIVER_RESTART"))
            if status is None or not len(status):
                if events is not None and len(events):
                    raise ValueError(f"Restart Event without receiver-telemetry status context: {day}")
                self.status_changes[day] = (session, changes)
                continue
            t, valid = ns(status["gpst"])
            u, uv = ns(status["receiver_uptime_s"])
            valid_indices = np.flatnonzero(valid)
            cursor = 0
            indices = []
            if events is not None and len(events):
                et, ev = ns(events["gpst"])
                eu, euv = ns(events["receiver_uptime_s"])
                for stamp, has_time, uptime, has_uptime in zip(et, ev, eu, euv):
                    match = uv & (u == uptime)
                    found = np.flatnonzero(match[cursor:]) if has_uptime else []
                    if not len(found):
                        raise ValueError(f"Cannot associate restart Event with its status report: {day}")
                    cursor += int(found[0])
                    indices.append(cursor)
                    cursor += 1
            self.status_changes[day] = (session, changes)
            # Only restart positions require Python work; ordinary status rows
            # are assigned sessions with searchsorted during the output pass.
            for end in indices + [len(t)]:
                if pending is not None:
                    following = valid_indices[valid_indices < end]
                    # pending begins after the preceding restart index.
                    following = following[following > pending[2]] if pending[3] == day else following
                    if len(following):
                        first = int(t[following[0]])
                        self.uncertain.append((pending[0], first))
                        self.starts.append((first, pending[1]))
                        pending = None
                before = valid_indices[valid_indices < end]
                if len(before):
                    last_time = int(t[before[-1]])
                if end == len(t):
                    break
                session += 1
                changes.append(end)
                if valid[end]:
                    if pending is not None:
                        self.uncertain.append((pending[0], int(t[end])))
                        pending = None
                    self.starts.append((int(t[end]), session))
                    last_time = int(t[end])
                else:
                    pending = (last_time, session, end, day)
            if len(valid_indices):
                last_time = int(t[valid_indices[-1]])
        if pending is not None:
            self.uncertain.append((pending[0], np.iinfo(np.int64).max))
        if any(a[0] > b[0] for a, b in zip(self.starts, self.starts[1:])):
            raise ValueError("Reversed receiver restart timeline")
        self.count = session

    def assign(self, times, valid):
        starts = np.array([x[0] for x in self.starts], dtype=np.int64)
        ids = np.array([0] + [x[1] for x in self.starts], dtype=np.int64)
        sessions = ids[np.searchsorted(starts, times, side="right")]
        known = valid.copy()
        for lower, upper in self.uncertain:
            known &= ~((times > lower) & (times < upper))
        sessions[~known] = -1
        return sessions, known


def add(table, name, values, dtype=None, mask=None):
    array = values if isinstance(values, (pa.Array, pa.ChunkedArray)) else pa.array(values, type=dtype, mask=mask, from_pandas=True)
    index = table.schema.get_field_index(name)
    return table.set_column(index, name, array) if index >= 0 else table.append_column(name, array)


class Writer:
    def __init__(self, path, config):
        self.path, self.config, self.writer = path, config, None

    def write(self, table):
        metadata = {
            **{k: v for k, v in (table.schema.metadata or {}).items() if not k.startswith(b"commonnex.")},
            **METADATA,
            b"input_format": b"CommonNEX",
            b"max_gap_seconds": str(self.config["max_gap"]).encode(),
            b"temperature_max_age_seconds": str(self.config["temperature_max_age"]).encode(),
            b"jump_tolerance_ns": str(self.config["jump_tolerance_ns"]).encode(),
            b"error_sign": b"actual edge minus ideal edge; positive late",
        }
        table = table.replace_schema_metadata(metadata)
        if self.writer is None:
            self.writer = pq.ParquetWriter(
                self.path, table.schema, compression="zstd", compression_level=3, use_dictionary=dictionary_columns(table.schema)
            )
        self.writer.write_table(table)

    def close(self):
        if self.writer:
            self.writer.close()


def empty_status():
    return pa.table(
        {
            "gpst_ns": pa.array([], pa.int64()),
            "receiver_session_id": pa.array([], pa.int64()),
            "temperature_c": pa.array([], pa.float32()),
        }
    )


def empty_pps():
    return pa.table(
        {
            "gpst_ns": pa.array([], pa.int64()),
            "quantization_error_ns": pa.array([], pa.float64()),
            "quantization_error_valid": pa.array([], pa.bool_()),
            "reference_time_scale": pa.array([], pa.string()),
        }
    )


def statistics(output, max_age):
    """Reduce derived samples without duplicating low-rate status in each row."""
    join = StatusJoin(output / "status.parquet", max_age)
    ranges, bins = {}, {}
    sums = np.zeros(6, dtype=np.longdouble)
    with pq.ParquetFile(output / "clock.parquet") as source:
        for batch in source.iter_batches(columns=["gpst_ns", "receiver_session_id", "clock_bias_ns", "clock_drift_ns_s"]):
            times = batch["gpst_ns"].fill_null(-1).to_numpy(zero_copy_only=False)
            valid = times >= 0
            t = times[valid]
            sid = batch["receiver_session_id"].to_numpy(zero_copy_only=False)[valid]
            temp = join.temperature(t, sid)
            drift = batch["clock_drift_ns_s"].to_numpy(zero_copy_only=False)[valid]
            for name, values in (
                ("gpst_ns", t),
                ("temperature_c", temp),
                ("clock_drift_ns_s", drift),
                ("clock_bias_ns", batch["clock_bias_ns"].to_numpy(zero_copy_only=False)[valid]),
            ):
                values = values[np.isfinite(values)]
                if len(values):
                    low, high = values.min().item(), values.max().item()
                    previous = ranges.get(name, {"min": low, "max": high})
                    ranges[name] = {"min": min(previous["min"], low), "max": max(previous["max"], high)}
            good = np.isfinite(temp) & np.isfinite(drift) & (sid >= 0)
            x, y = temp[good].astype(np.longdouble), drift[good].astype(np.longdouble)
            sums += [len(x), x.sum(), y.sum(), (x * x).sum(), (y * y).sum(), (x * y).sum()]
            degrees = np.trunc(x).astype(np.int64)
            for degree in np.unique(degrees):
                d = y[degrees == degree]
                item = bins.setdefault(int(degree), np.zeros(3, dtype=np.longdouble))
                item += [len(d), d.sum(), (d * d).sum()]
    n, sx, sy, xx, yy, xy = sums
    vx, vy = n * xx - sx * sx, n * yy - sy * sy
    correlation = float((n * xy - sx * sy) / np.sqrt(vx * vy)) if n and vx > 0 and vy > 0 else None
    return dict(
        ranges=ranges,
        samples_with_temperature=int(n),
        temperature_drift_pearson_r=correlation,
        temperature_drift_bins=[
            dict(
                temperature_c=k,
                count=int(v[0]),
                mean_drift_ns_s=float(v[1] / v[0]),
                stddev_drift_ns_s=float(np.sqrt(max(0, v[2] / v[0] - (v[1] / v[0]) ** 2))),
            )
            for k, v in sorted(bins.items())
        ],
    )


@click.command()
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="CommonNEX logical station containing setup.json and GPST day catalogs.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--reference-time-scale", default="GPST", show_default=True, help="Clock estimate reference, not its GPST time axis.")
@click.option("--max-gap", type=click.FloatRange(min=0, min_open=True), default=50.0, show_default=True)
@click.option("--jump-tolerance-ns", type=click.IntRange(1, 499999), default=50000, show_default=True)
@click.option("--temperature-max-age", type=click.FloatRange(min=0), default=5.0, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success, retaining a backup.")
@staged_output
def cli(input_dir, output, reference_time_scale, max_gap, jump_tolerance_ns, temperature_max_age):
    """Analyze receiver clock from CommonNEX; no UBX/SBF parsing or timestamp reconstruction."""
    try:
        root = input_dir.resolve()
        setup = validate_setup(json.loads((root / "setup.json").read_text()))
        days = list(day_directories(root))
        if not days:
            raise ValueError("No CommonNEX day partitions")
        sessions = Sessions(days, setup["setup_id"])
        tracker = BatchUnwrapper(max_gap, jump_tolerance_ns)
        counts = Counter(status=0, pps=0, excluded_clock_rows=0)
        config = dict(
            max_gap=max_gap,
            jump_tolerance_ns=jump_tolerance_ns,
            temperature_max_age=temperature_max_age,
            reference_time_scale=reference_time_scale,
            input_format="CommonNEX",
        )
        output.mkdir(exist_ok=False)
        with ExitStack() as stack:
            writers = {key: Writer(output / f"{key}.parquet", config) for key in ("clock", "status", "pps")}
            for writer in writers.values():
                stack.callback(writer.close)
            for day in tqdm(days, desc="Receiver clock", unit="day"):
                telemetry = catalog(day, "receiver-telemetry", setup["setup_id"])
                status = telemetry_status(telemetry)
                if status is not None:
                    t, valid = ns(status["gpst"])
                    base, changes = sessions.status_changes[day]
                    sid = base + np.searchsorted(changes, np.arange(len(status)), side="right")
                    status = add(status, "gpst_ns", t, pa.int64(), ~valid)
                    status = add(status, "receiver_session_id", sid, pa.int64())
                    status = add(status, "temperature_c", status["receiver_temperature_c"])
                    status = add(status, "restart", np.isin(np.arange(len(status)), changes), pa.bool_())
                    writers["status"].write(status)
                    counts["status"] += len(status)
                pulses = telemetry_pulses(telemetry)
                if pulses is not None:
                    t, valid = ns(pulses["gpst"])
                    pulses = add(pulses, "gpst_ns", t, pa.int64(), ~valid)
                    pulses = add(pulses, "quantization_error_ns", nanoseconds(pulses, "pulse_quantization_error_s"), pa.float64())
                    writers["pps"].write(pulses)
                    counts["pps"] += len(pulses)
                if telemetry is None or not len(telemetry):
                    continue
                clock = telemetry.rename_columns(
                    ["reference_time_scale" if n == "clock_reference_time_scale" else n for n in telemetry.column_names]
                )
                selected = pc.fill_null(pc.equal(clock["reference_time_scale"], reference_time_scale), False)
                counts["excluded_clock_rows"] += len(clock) - pc.sum(pc.cast(selected, pa.int64())).as_py()
                clock = clock.filter(selected)
                if not len(clock):
                    continue
                for batch in clock.to_batches(max_chunksize=65536):
                    table = pa.Table.from_batches([batch])
                    t, valid = ns(table["gpst"])
                    sid, known = sessions.assign(t, valid)
                    table = add(table, "gpst_ns", t, pa.int64(), ~valid)
                    table = add(table, "receiver_session_id", sid, pa.int64())
                    table = add(table, "continuity_known", known, pa.bool_())
                    table = add(table, "clock_bias_ns", nanoseconds(table, "clock_bias_s"), pa.float64())
                    table = add(table, "clock_drift_ns_s", numeric(table, "clock_frequency_offset") / 1e6, pa.float64())
                    table = add(table, "time_accuracy_ns", nanoseconds(table, "time_accuracy_s"), pa.float64())
                    table = add(table, "frequency_accuracy_ps_s", numeric(table, "frequency_accuracy") / 1e3, pa.float64())
                    flags, counters, moduli = measurement_evidence(table["measurement_clock"])
                    table = add(table, "rawx_clock_reset", flags)
                    table = add(table, "cumulative_clock_jumps_ms", counters)
                    table = add(table, "cumulative_clock_jumps_modulus_ms", moduli)
                    for name, dtype in (
                        ("clock_arc_id", pa.int64()),
                        ("clock_adjustment_total_ns", pa.int64()),
                        ("clock_bias_unwrapped_ns", pa.float64()),
                        ("adjustment_ns", pa.int64()),
                        ("adjustment_evidence", pa.string()),
                        ("arc_start_reason", pa.string()),
                    ):
                        table = add(table, name, pa.nulls(len(table), dtype))
                    writers["clock"].write(tracker.apply(table))
            if writers["clock"].writer is None:
                raise ValueError("No receiver-clock estimates match the selection; import telemetry first")
            if writers["status"].writer is None:
                writers["status"].write(empty_status())
            if writers["pps"].writer is None:
                writers["pps"].write(empty_pps())
        summary = dict(
            status="complete",
            input_format="CommonNEX",
            setup_id=setup["setup_id"],
            config=config,
            time_scale="GPST",
            counts={**counts, **tracker.counts, "receiver_restarts": sessions.count},
        )
        summary.update(statistics(output, temperature_max_age))
        write_json(output / "summary.json", summary)
        click.echo(json.dumps(summary))
    except (OSError, ValueError, KeyError, RuntimeError, pa.ArrowException) as exc:
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
