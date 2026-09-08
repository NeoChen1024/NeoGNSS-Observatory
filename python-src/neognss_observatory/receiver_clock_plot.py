# SPDX-License-Identifier: GPL-3.0-only
"""Bounded-memory clock plot preparation and parallel, headless PNG rendering."""

import json
import multiprocessing
import os
from collections import defaultdict
from pathlib import Path

import click
import matplotlib
import numpy as np
import pyarrow.parquet as pq
from tqdm import tqdm

from .clock_status import StatusJoin
from .research_output import staged_output

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from .gpst import calendar, label
from .sbas_extract import write_json

HOUR_NS = 3600 * 10**9
COLUMNS = (
    "gpst_ns",
    "clock_arc_id",
    "clock_bias_ns",
    "clock_bias_unwrapped_ns",
    "clock_drift_ns_s",
    "time_accuracy_ns",
    "frequency_accuracy_ps_s",
    "receiver_session_id",
)
SERIES = ("bias", "unwrapped", "drift", "tacc", "facc", "temperature")


def extrema_indices(data, bins=900, event_times=()):
    """Keep endpoints/extrema, arc boundaries and both sides of adjustments."""
    n = len(data["t"])
    if n <= bins:
        return np.arange(n)
    bucket = np.minimum((data["t"] * bins / 3600).astype(int), bins - 1)
    edges = np.r_[0, np.flatnonzero(np.diff(bucket)) + 1, n]
    selected = set(edges[:-1].tolist()) | set((edges[1:] - 1).tolist())
    positions = np.arange(n)
    for name in SERIES:
        values = data[name]
        valid = np.isfinite(values)
        for reduction, missing in ((np.minimum, np.inf), (np.maximum, -np.inf)):
            extreme = reduction.reduceat(np.where(valid, values, missing), edges[:-1])
            matches = valid & (values == np.repeat(extreme, np.diff(edges)))
            indices = np.minimum.reduceat(np.where(matches, positions, n), edges[:-1])
            selected.update(indices[indices < n].tolist())
    breaks = np.flatnonzero(np.diff(data["arc"]) != 0)
    selected.update(breaks.tolist())
    selected.update((breaks + 1).tolist())
    for t in event_times:
        i = int(np.searchsorted(data["t"], t))
        selected.update(j for j in (i - 1, i, i + 1) if 0 <= j < n)
    return np.array(sorted(selected), dtype=int)


def event_inventory(path):
    grouped = defaultdict(list)
    for batch in pq.ParquetFile(path).iter_batches(columns=["gpst_ns", "adjustment_ns", "adjustment_evidence"]):
        for row in batch.to_pylist():
            if row["gpst_ns"] is not None and row["adjustment_ns"]:
                grouped[row["gpst_ns"] // HOUR_NS].append(
                    dict(kind="clock_adjustment", gpst_ns=row["gpst_ns"], evidence=row["adjustment_evidence"])
                )
    return grouped


def prepare(source, output, max_gap):
    """Read selected Parquet columns once; buffer at most one hour plus a batch."""
    parquet = pq.ParquetFile(source / "clock.parquet")
    if parquet.schema_arrow.metadata.get(b"time_scale") != b"GPST":
        raise ValueError("Expected GPST samples")
    events = event_inventory(source / "clock.parquet")
    status = StatusJoin(source / "status.parquet", float(parquet.schema_arrow.metadata[b"temperature_max_age_seconds"]))
    cache = output / "cache"
    cache.mkdir()
    hours, pending, current = [], [], None
    previous_time, previous_arc = None, None
    skipped = 0

    def flush():
        if not pending:
            return
        data = {key: np.concatenate([part[key] for part in pending]) for key in pending[0]}
        hourly_events = events.get(current, [])
        adjustments = [e for e in hourly_events if e["kind"] == "clock_adjustment"]
        event_t = [(e["gpst_ns"] - current * HOUR_NS) / 1e9 for e in adjustments]
        exposure = float(np.sum(data["exposure"]))
        temp = data["temperature"][np.isfinite(data["temperature"])]
        entry = {
            "hour_gpst": current * 3600,
            "samples": len(data["t"]),
            "exposure_s": exposure,
            "arcs": len(np.unique(data["arc"])),
            "adjustments": len(adjustments),
            "inferred_adjustments": sum(e.get("evidence") == "bias_inferred_adjustment" for e in adjustments),
            "confirmed_adjustments": sum(e.get("evidence") == "rawx_confirmed_adjustment" for e in adjustments),
            "counted_adjustments": sum(e.get("evidence") == "sbf_counted_adjustment" for e in adjustments),
            "adjustments_per_observed_hour": len(adjustments) * 3600 / exposure if exposure > 0 else None,
            "drift_p10_p50_p90": np.quantile(data["drift"], [0.1, 0.5, 0.9]).tolist(),
            "temperature_median_c": float(np.median(temp)) if len(temp) else None,
        }
        indices = extrema_indices(data, event_times=event_t)
        path = cache / (label(current * 3600) + ".npz")
        # Statistics use all samples; only the visual representation is reduced.
        np.savez_compressed(
            path,
            **{k: v[indices] for k, v in data.items() if k != "exposure"},
            adjustment_t=np.array(event_t),
            confirmed=np.array([e.get("evidence") in ("rawx_confirmed_adjustment", "sbf_counted_adjustment") for e in adjustments]),
        )
        entry["cache"] = path.name
        hours.append(entry)
        pending.clear()

    with tqdm(total=parquet.metadata.num_rows, desc="Clock plot inventory", unit="sample", unit_scale=True) as progress:
        for batch in parquet.iter_batches(batch_size=131072, columns=COLUMNS):
            values = {name: batch.column(name).to_numpy(zero_copy_only=False) for name in COLUMNS if name != "gpst_ns"}
            timestamps = batch.column("gpst_ns").fill_null(-1).to_numpy(zero_copy_only=False)
            good = timestamps >= 0
            skipped += int(np.count_nonzero(~good))
            t = timestamps[good]
            values = {k: v[good] for k, v in values.items()}
            progress.update(batch.num_rows)
            if not len(t):
                continue
            values["temperature_c"] = status.temperature(t, values["receiver_session_id"])
            if np.any(np.diff(t) <= 0) or previous_time is not None and t[0] <= previous_time:
                raise ValueError("Plot input has non-increasing GPST")
            arc = values["clock_arc_id"].astype(np.int64)
            bias = values["clock_bias_unwrapped_ns"].astype(float)
            prior_t = np.r_[previous_time if previous_time is not None else t[0], t[:-1]]
            prior_arc = np.r_[previous_arc if previous_arc is not None else arc[0], arc[:-1]]
            dt = (t - prior_t) / 1e9
            exposure = np.where((arc == prior_arc) & (dt > 0) & (dt <= max_gap), dt, 0)
            previous_time, previous_arc = int(t[-1]), int(arc[-1])
            hour = t // HOUR_NS
            edges = np.r_[0, np.flatnonzero(np.diff(hour)) + 1, len(t)]
            for begin, end in zip(edges[:-1], edges[1:]):
                number = int(hour[begin])
                if current is not None and current != number:
                    flush()
                current = number
                sl = slice(begin, end)
                pending.append(
                    {
                        "t": (t[sl] - number * HOUR_NS) / 1e9,
                        "arc": arc[sl],
                        "bias": values["clock_bias_ns"][sl] / 1000,
                        "unwrapped": bias[sl] / 1000,
                        "drift": values["clock_drift_ns_s"][sl],
                        "tacc": values["time_accuracy_ns"][sl],
                        "facc": values["frequency_accuracy_ps_s"][sl],
                        "temperature": values["temperature_c"][sl],
                        "exposure": exposure[sl],
                    }
                )
        flush()
    write_json(output / "hours.json", {"hours": hours, "unassigned_samples_excluded": skipped})
    return hours


def line_with_breaks(ax, data, key, **kwargs):
    # Sampling reduction may skip ordinary epochs, so break by arc, not by the
    # distance between retained visual points. Arcs come from the extractor.
    breaks = np.flatnonzero(np.diff(data["arc"]) != 0) + 1
    x = np.insert(data["t"] / 60, breaks, np.nan)
    y = np.insert(np.asarray(data[key], dtype=float), breaks, np.nan)
    ax.plot(x, y, linewidth=0.7, **kwargs)


def unavailable(ax):
    ax.text(0.5, 0.5, "unavailable", ha="center", va="center", transform=ax.transAxes, color="#777777", fontsize=15)


def save_figure(fig, path):
    temporary = path.with_suffix(".partial.png")
    fig.savefig(temporary, dpi=100, facecolor="white", pil_kwargs={"compress_level": 1})
    plt.close(fig)
    os.replace(temporary, path)
    return {"name": str(path.name)}


def render_hour(job):
    root, title, entry = job
    root = Path(root)
    with np.load(root / "cache" / entry["cache"]) as data:
        fig, axes = plt.subplots(5, 1, figsize=(16, 12), sharex=True, layout="constrained")
        fig.suptitle(
            f"{title} | {calendar(entry['hour_gpst']):%Y-%m-%d %H:00} GPST\n"
            f"{entry['samples']:,} samples | {entry['arcs']} arcs | "
            f"{entry['inferred_adjustments']} inferred / {entry['confirmed_adjustments']} RAWX-confirmed / "
            f"{entry['counted_adjustments']} SBF-counted adjustments"
        )
        for ax, key, ylabel, color in zip(
            axes[:3],
            SERIES[:3],
            ("Raw bias (µs)", "Unwrapped bias (µs)", "Clock drift (ns/s)"),
            ("#2563a0", "#4d7a32", "#7554a3"),
        ):
            line_with_breaks(ax, data, key, color=color)
            ax.set_ylabel(ylabel)
        for t, confirmed in zip(data["adjustment_t"], data["confirmed"]):
            axes[0].axvline(t / 60, color="#15803d" if confirmed else "#d97706", linewidth=0.7, alpha=0.7)
        line_with_breaks(axes[3], data, "tacc", color="#2563a0")
        axes[3].set_ylabel("tAcc (ns)", color="#2563a0")
        other = axes[3].twinx()
        line_with_breaks(other, data, "facc", color="#b45309")
        other.set_ylabel("fAcc (ps/s)", color="#b45309")
        for ax in (axes[3], other):
            ax.set_yscale("symlog", linthresh=1)
        if not np.any(np.isfinite(data["tacc"])) and not np.any(np.isfinite(data["facc"])):
            unavailable(axes[3])
        axes[4].set_ylabel("Receiver temperature (°C)")
        if np.any(np.isfinite(data["temperature"])):
            line_with_breaks(axes[4], data, "temperature", color="#b45309")
        else:
            unavailable(axes[4])
        for ax in axes:
            ax.grid(alpha=0.2)
            ax.set_xlim(0, 60)
        axes[-1].set_xlabel("Minutes from start of GPST hour; arc boundaries are not connected")
        return save_figure(fig, root / "hourly" / (label(entry["hour_gpst"]) + ".png"))


def prepare_pps(source, output):
    path = source / "pps.parquet"
    if not path.exists():
        return []
    jobs, pending = [], []
    current = None

    def flush():
        if not pending:
            return
        data = np.concatenate(pending)
        name = label(current * 3600)
        cache = output / "cache" / f"pps-{name}.npz"
        np.savez_compressed(cache, data=data)
        jobs.append((str(cache), str(output / "pps" / f"{name}.png"), current))
        pending.clear()

    for batch in pq.ParquetFile(path).iter_batches(batch_size=65536):
        rows = batch.to_pydict()
        valid = np.array([t is not None for t in rows["gpst_ns"]])
        times = np.array([t or 0 for t in rows["gpst_ns"]], dtype=np.int64)[valid]
        errors = np.array(rows["quantization_error_ns"], dtype=float)[valid]
        references = np.array(rows["reference_time_scale"], dtype=str)[valid]
        for hour in np.unique(times // HOUR_NS):
            if current is not None and hour != current:
                if hour < current:
                    raise ValueError("Non-monotonic PPS hour sequence")
                flush()
            current = int(hour)
            mask = times // HOUR_NS == hour
            pending.append(
                np.rec.fromarrays([(times[mask] - hour * HOUR_NS) / 1e9, errors[mask], references[mask]], names="t,error,reference")
            )
    flush()
    return jobs


def render_pps(job):
    cache, path, hour = job
    with np.load(cache) as archive:
        data = archive["data"]
        fig, ax = plt.subplots(figsize=(16, 5), layout="constrained")
        for ref in np.unique(data["reference"]):
            selected = data[data["reference"] == ref]
            ax.scatter(selected["t"] / 60, selected["error"], s=3, label=ref)
        if not np.any(np.isfinite(data["error"])):
            unavailable(ax)
        ax.set(
            title=f"PPS quantization error | {calendar(hour * 3600):%Y-%m-%d %H:00} GPST\n"
            f"{len(data):,} reports; {np.count_nonzero(~np.isfinite(data['error'])):,} unavailable",
            xlabel="Minutes from start of GPST hour; legend identifies pulse reference scale",
            ylabel="Receiver-reported error (ns)",
            xlim=(0, 60),
        )
        ax.grid(alpha=0.2)
        ax.legend()
        return save_figure(fig, Path(path))


def render_day(job):
    root, title, day, entries = job
    fig, axes = plt.subplots(4, 1, figsize=(16, 10), sharex=True, layout="constrained")
    fig.suptitle(f"{title} | {calendar(day):%Y-%m-%d} GPST | daily clock overview")
    q = np.full((24, 3), np.nan)
    rate, coverage, arcs, temp = (np.full(24, np.nan) for _ in range(4))
    for e in entries:
        h = (e["hour_gpst"] - day) // 3600
        q[h] = e["drift_p10_p50_p90"]
        rate[h] = e["adjustments_per_observed_hour"] if e["adjustments_per_observed_hour"] is not None else np.nan
        coverage[h], arcs[h] = e["exposure_s"] / 3600 * 100, e["arcs"]
        temp[h] = e["temperature_median_c"] if e["temperature_median_c"] is not None else np.nan
    x = np.arange(24) + 0.5
    axes[0].fill_between(x, q[:, 0], q[:, 2], alpha=0.2, color="#7554a3", label="Hourly P10–P90")
    axes[0].plot(x, q[:, 1], ".-", color="#7554a3", label="Hourly median")
    axes[0].set_ylabel("Clock drift (ns/s)")
    axes[0].legend(loc="upper right")
    axes[1].bar(x, rate, width=0.8, color="#d97706")
    axes[1].set_ylabel("Adjustments /\nobserved hour")
    axes[2].bar(x, coverage, width=0.8, color="#2563a0", alpha=0.7)
    axes[2].set_ylabel("Observed interval\ncoverage (%)")
    axes[2].set_ylim(0, max(100, np.nanmax(coverage) if np.any(np.isfinite(coverage)) else 100))
    other = axes[2].twinx()
    other.plot(x, arcs, ".", color="#b45309")
    other.set_ylabel("Arcs present", color="#b45309")
    axes[3].set_ylabel("Median temperature (°C)")
    if np.any(np.isfinite(temp)):
        axes[3].plot(x, temp, ".-", color="#b45309")
    else:
        unavailable(axes[3])
    for ax in axes:
        ax.grid(alpha=0.2)
        ax.set_xlim(0, 24)
        ax.set_xticks(np.arange(0, 25, 2))
    axes[-1].set_xlabel("GPST hour; blank intervals have no data, not zero measurements")
    return save_figure(fig, Path(root) / "daily" / (label(day) + ".png"))


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="New plot output directory.")
@click.option("--title", default="Receiver clock", show_default=True)
@click.option("--workers", type=click.IntRange(1, 32), default=8, show_default=True)
@click.option("--hourly/--no-hourly", default=True, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, title, workers, hourly):
    """Render hourly clock details and daily summaries from completed telemetry."""
    try:
        source, output = input_dir.resolve(), output.resolve()
        metadata = pq.ParquetFile(source / "clock.parquet").schema_arrow.metadata or {}
        if metadata.get(b"time_scale") != b"GPST":
            raise ValueError("Expected GPST clock samples")
        max_gap = float(metadata[b"max_gap_seconds"])
        output.mkdir(parents=True, exist_ok=False)
        (output / "hourly").mkdir()
        (output / "daily").mkdir()
        (output / "pps").mkdir()
        hours = prepare(source, output, max_gap)
        pps_jobs = prepare_pps(source, output) if hourly else []
        days = defaultdict(list)
        for entry in hours:
            days[entry["hour_gpst"] // 86400 * 86400].append(entry)
        results = []
        with multiprocessing.get_context("spawn").Pool(workers) as pool:
            for result in tqdm(pool.imap_unordered(render_pps, pps_jobs), total=len(pps_jobs), desc="PPS PNG", unit="image"):
                results.append(dict(result, directory="pps"))
            day_jobs = [(str(output), title, day, entries) for day, entries in sorted(days.items())]
            for result in tqdm(
                pool.imap_unordered(render_day, day_jobs), total=len(day_jobs), desc="Daily clock PNG", unit="image"
            ):
                results.append(dict(result, directory="daily"))
            if hourly:
                jobs = [(str(output), title, entry) for entry in hours]
                for result in tqdm(
                    pool.imap_unordered(render_hour, jobs, chunksize=4), total=len(jobs), desc="Hourly clock PNG", unit="image"
                ):
                    results.append(dict(result, directory="hourly"))
        write_json(output / "images.json", {"images": sorted(results, key=lambda r: (r["directory"], r["name"]))})
        write_json(
            output / "summary.json",
            {
                "status": "complete",
                "hours": len(hours),
                "days": len(days),
                "images": len(results),
                "temperature": "available" if any(e["temperature_median_c"] is not None for e in hours) else "unavailable",
            },
        )
        click.echo(json.dumps({"status": "complete"}))
    except (OSError, ValueError, KeyError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
