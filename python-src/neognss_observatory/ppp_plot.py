# SPDX-License-Identifier: GPL-3.0-only
"""Plot the numerical PPP Float outputs without reopening observations."""

import json
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from .gpst import calendar
from .ppp_report import write_report
from .research_output import staged_output


def read_columns(directory):
    paths = sorted(directory.glob("GPST-*.parquet"))
    if not paths:
        raise ValueError(f"No PPP Parquet files in {directory}")
    table = pa.concat_tables([pq.read_table(path) for path in paths])
    return np.rec.fromarrays([table[name].to_numpy() for name in table.column_names], names=table.column_names)


def by_satellite(rows):
    for prn in np.unique(rows.prn):
        yield prn, rows[rows.prn == prn]


def render_figure(source, kind):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    epochs = read_columns(source / "epochs")
    satellites = read_columns(source / "satellites")
    summary = json.loads((source / "summary.json").read_text())
    start_ns, end_ns = int(epochs.gpst_ns[0]), int(epochs.gpst_ns[-1])

    def hours(times):
        return (times - start_ns) / 3_600_000_000_000

    t = hours(epochs.gpst_ns)
    start_label = calendar(start_ns / 1e9).strftime("%Y-%m-%d %H:%M:%S")
    end_label = calendar(end_ns / 1e9).strftime("%Y-%m-%d %H:%M:%S")
    title = f"{summary['station']} | GPS static forward PPP Float\n{start_label} – {end_label} GPST"
    if kind == "sky":
        fig, ax = plt.subplots(figsize=(10, 10), subplot_kw=dict(projection="polar"))
        ax.set_theta_zero_location("N")
        ax.set_theta_direction(-1)
        ax.set_thetagrids(range(0, 360, 45), ["N", "NE", "E", "SE", "S", "SW", "W", "NW"])
        for prn, rows in by_satellite(satellites):
            rows = rows[rows.elevation_deg >= 0]
            ax.scatter(np.deg2rad(rows.azimuth_deg), 90 - rows.elevation_deg, s=2, label=f"G{prn:02d}")
        ax.set_ylim(0, 90)
        ax.set_yticks([0, 30, 60, 90], ["90°", "60°", "30°", "0° elevation"])
        ax.set_title(title + "\nSelected dual-frequency observations (not all tracked satellites)", pad=30)
        ax.legend(loc="upper left", bbox_to_anchor=(1.1, 1), ncol=2, fontsize=8)
    elif kind in ("position", "position-settled"):
        fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
        final = summary["final_epoch_solution"]
        if kind == "position-settled":
            cutoff = summary["start_gpst_ns"] + 3600_000_000_000
            epochs = epochs[epochs.gpst_ns >= cutoff]
            t = hours(epochs.gpst_ns)
        for ax, name, sigma, label in zip(
            axes, ("east", "north", "up"), ("sigma_e", "sigma_n", "sigma_u"), ("East", "North", "Up")
        ):
            ax.plot(t, epochs[name], lw=0.8, label="Epoch solution")
            ax.fill_between(
                t, epochs[name] - 1.96 * epochs[sigma], epochs[name] + 1.96 * epochs[sigma], alpha=0.2, label="Formal 95% (1.96σ)"
            )
            ax.axhline(final[name], color="orange", label="Last valid static estimate")
            ax.set_ylabel(f"{label} − a priori (m)")
            ax.grid(alpha=0.25)
        axes[0].legend()
        note = (
            "First hour omitted for detail; no automatic convergence claim"
            if kind == "position-settled"
            else "Full interval including initialization"
        )
        axes[0].set_title(title + "\n" + note)
    elif kind == "states":
        fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
        axes[0].plot(t, epochs.ztd_m, lw=1)
        axes[0].fill_between(
            t, epochs.ztd_m - epochs.ztd_sigma_m, epochs.ztd_m + epochs.ztd_sigma_m, alpha=0.25, label="Formal ±1σ"
        )
        axes[0].set_ylabel("Estimated ZTD (m)")
        axes[0].legend()
        axes[1].plot(t, epochs.clock_ns * 0.001, lw=0.7)
        axes[1].set_ylabel("GPS receiver clock (µs)")
        axes[1].set_title("Raw PPP clock; jumps are not unwrapped")
        clock_sigma = axes[1].twinx()
        clock_sigma.plot(t, epochs.clock_sigma_ns, color="tab:red", lw=0.7)
        clock_sigma.set_ylabel("Formal clock 1σ (ns)", color="tab:red")
        axes[2].plot(t, epochs.satellites, label="Used in valid PPP solution")
        times, counts = np.unique(satellites.gpst_ns, return_counts=True)
        axes[2].plot(hours(times), counts, label="Selected dual-frequency GPS")
        axes[2].set_ylabel("Satellites")
        axes[2].legend()
        axes[0].set_title(title)
        for ax in axes:
            ax.grid(alpha=0.25)
    else:
        fig, axes = plt.subplots(2, 1, figsize=(14, 10), sharex=True)
        for prn, rows in by_satellite(satellites):
            time = hours(rows.gpst_ns)
            axes[0].scatter(time, rows.phase_residual_m, s=2, label=f"G{prn:02d}")
            axes[1].scatter(time, rows.code_residual_m, s=2)
        axes[0].set_ylabel("Phase residual (m)")
        axes[1].set_ylabel("Code residual (m)")
        axes[0].set_title(title + "\nPost-fit ionosphere-free L1/L2 residuals (used observations)")
        axes[0].legend(ncol=8, fontsize=8)
        for ax in axes:
            ax.grid(alpha=0.25)
    if kind != "sky":
        axes[-1].set_xlim(0, max(float(hours(end_ns)), 1 / 3600))
        axes[-1].set_xlabel(f"Elapsed hours from {start_label} GPST")
    fig.set_layout_engine("constrained")
    return fig


def draw(task):
    import matplotlib.pyplot as plt

    source, output, kind = task
    fig = render_figure(source, kind)
    path = output / f"solution-{kind}.png"
    fig.savefig(path, dpi=120, pil_kwargs=dict(compress_level=3))
    plt.close(fig)
    return path.name


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--workers", type=click.IntRange(min=1), default=2, show_default=True)
@click.option("--overwrite", is_flag=True)
@staged_output
def cli(input_dir, output, workers):
    """Render whole-solution PPP PNGs in parallel and a PDF report."""
    paths = sorted((input_dir / "epochs").glob("GPST-*.parquet"))
    if not paths:
        raise click.ClickException("No PPP epoch Parquet files")
    output.mkdir()
    summary = json.loads((input_dir / "summary.json").read_text())
    kinds = ["position", "states", "sky", "residuals"]
    if summary["end_gpst_ns"] - summary["start_gpst_ns"] > 3_600_000_000_000:
        kinds.insert(1, "position-settled")
    tasks = [(input_dir, output, kind) for kind in kinds]
    with ProcessPoolExecutor(max_workers=workers) as pool:
        list(tqdm(pool.map(draw, tasks), total=len(tasks), desc="PPP plots", unit="figure"))
    report = write_report(input_dir, output, kinds, render_figure)
    click.echo(f"PDF report: {report.name}", err=True)


if __name__ == "__main__":
    cli()
