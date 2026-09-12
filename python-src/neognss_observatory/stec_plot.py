# SPDX-License-Identifier: GPL-3.0-only
"""Hourly absolute STEC IPP trajectories from finalized Parquet only."""

import json
import math
import multiprocessing
import os
import tempfile
import zipfile
from collections import defaultdict
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from pathlib import Path

import click
import numpy as np
import pyarrow.parquet as pq
from tqdm import tqdm

from .gpst import calendar, label
from .research_output import staged_output, write_json
from .sbas_grid_parquet import SCHEMA as SBAS_SCHEMA
from .sbas_grid_plot import hourly_rows
from .sbas_grid_render import coastline_parts, parse_hour
from .stec import read_stec

HOUR_NS = 3_600_000_000_000
FIELDS = (
    "gpst_ns",
    "arc_id",
    "prn",
    "receiver_window_id",
    "ipp_latitude_deg",
    "ipp_longitude_deg",
    "stec_absolute_tecu",
)
_worker = None


def station_position(xyz):
    x, y, z = np.asarray(xyz, dtype=float)
    if not np.isfinite([x, y, z]).all() or not 6e6 < np.linalg.norm([x, y, z]) < 7e6:
        raise ValueError("Invalid station ECEF coordinates in STEC metadata")
    rho, e2 = np.hypot(x, y), 6.6943799901413165e-3
    latitude = np.arctan2(z, rho * (1 - e2))
    for _ in range(10):
        radius = 6378137 / np.sqrt(1 - e2 * np.sin(latitude) ** 2)
        latitude = np.arctan2(z + e2 * radius * np.sin(latitude), rho)
    return float(np.rad2deg(np.arctan2(y, x))), float(np.rad2deg(latitude))


def wrap(longitude, center):
    return center + (np.asarray(longitude) - center + 180) % 360 - 180


def prepare_hours(root, scratch, start, end, center):
    """Spool at most one GPST hour in memory, preserving unavailable rows."""
    records, pending = [], []
    current = previous = None
    bounds = [math.inf, -math.inf, math.inf, -math.inf]

    def flush():
        data = {k: np.concatenate([p[k] for p in pending]) for k in FIELDS}
        path = scratch / f"{label(current//10**9)}.npz"
        np.savez(path, **data)
        records.append((current // 10**9, path))
        pending.clear()

    for table in tqdm(read_stec(root, start, end), desc="Read finalized STEC", unit="batch"):
        points = {k: table[k].to_numpy() for k in FIELDS}
        times = points["gpst_ns"]
        if np.any(np.diff(times) < 0) or (previous is not None and times[0] < previous):
            raise ValueError("Non-monotonic STEC sample times")
        previous = int(times[-1])
        points["ipp_longitude_deg"] = wrap(points["ipp_longitude_deg"], center)
        lon, lat = points["ipp_longitude_deg"], points["ipp_latitude_deg"]
        finite = np.isfinite(lon) & np.isfinite(lat)
        if np.any(finite):
            bounds = [
                min(bounds[0], lon[finite].min()),
                max(bounds[1], lon[finite].max()),
                min(bounds[2], lat[finite].min()),
                max(bounds[3], lat[finite].max()),
            ]
        hours = times // HOUR_NS * HOUR_NS
        for hour in np.unique(hours):
            if current is not None and hour != current:
                flush()
            current = int(hour)
            pending.append({k: v[hours == hour] for k, v in points.items()})
    if pending:
        flush()
    if not records:
        raise ValueError("No STEC samples in the requested GPST interval")
    return records, bounds


class Background:
    """Read each matching SBAS day once; never open raw subframes."""

    def __init__(self, root, hours, prn, coverage):
        self.files, self.rows, self.day = {}, {}, None
        self.prn, self.coverage = prn, coverage
        if root is None:
            return
        daily = root / "daily" if (root / "daily").is_dir() else root
        needed = {h // 86400 * 86400000 for h in hours}
        for path in daily.glob("GPST-*.parquet"):
            schema = pq.ParquetFile(path).schema_arrow
            meta = schema.metadata or {}
            if (
                not schema.equals(SBAS_SCHEMA)
                or any(meta.get(k) != v for k, v in SBAS_SCHEMA.metadata.items())
                or b"day_gpst_ms" not in meta
            ):
                raise ValueError(
                    f"Expected current SBAS grid Parquet: {path}; regenerate using ngo-sbas-frame-parquet and ngo-sbas-grid-parquet"
                )
            day = int(meta[b"day_gpst_ms"])
            if day in needed:
                if day in self.files:
                    raise ValueError("Duplicate SBAS day")
                self.files[day] = path

    def get(self, hour):
        day = hour // 86400 * 86400000
        if self.day != day:
            self.rows = defaultdict(list)
            self.day = day
            if day in self.files:
                for row in hourly_rows(self.files[day], day):
                    if (row["constellation"], row["signal"], row["prn"]) == ("SBAS", "L1CA", self.prn) and row[
                        "coverage"
                    ] >= self.coverage:
                        self.rows[row["hour_gpst"]].append(row)
        return self.rows.get(hour, [])


def initialize(coastline, options):
    global _worker
    coast = []
    west, east, south, north = options["extent"]
    for part in coastline_parts(coastline):
        vertices = np.asarray(part, dtype=float)
        vertices[:, 0] = wrap(vertices[:, 0], options["station"][0])
        for segment in np.split(vertices, np.flatnonzero(np.abs(np.diff(vertices[:, 0])) > 180) + 1):
            if (
                len(segment) > 1
                and segment[:, 0].max() >= west
                and segment[:, 0].min() <= east
                and segment[:, 1].max() >= south
                and segment[:, 1].min() <= north
            ):
                coast.append(segment)
    _worker = coast, options


def track_pieces(data, gap_ns):
    """No joins over missing calibration, arc/window changes, gaps or dateline."""
    for arc in np.unique(data["arc_id"]):
        indices = np.flatnonzero(data["arc_id"] == arc)
        valid = np.isfinite(data["stec_absolute_tecu"][indices]) & np.isfinite(data["receiver_window_id"][indices])
        valid &= np.isfinite(data["ipp_latitude_deg"][indices]) & np.isfinite(data["ipp_longitude_deg"][indices])
        times = data["gpst_ns"][indices]
        joined = valid[:-1] & valid[1:] & (np.diff(times) > 0) & (np.diff(times) <= gap_ns)
        joined &= np.diff(data["receiver_window_id"][indices]) == 0
        joined &= np.abs(np.diff(data["ipp_longitude_deg"][indices])) <= 180
        for chunk in np.split(np.arange(len(indices)), np.flatnonzero(~joined) + 1):
            chunk = indices[chunk]
            if len(chunk) and np.isfinite(data["stec_absolute_tecu"][chunk[0]]) and valid[np.searchsorted(indices, chunk[0])]:
                yield chunk


def render_hour(job):
    import matplotlib
    import matplotlib.pyplot as plt
    from matplotlib.cm import ScalarMappable
    from matplotlib.collections import LineCollection, PatchCollection
    from matplotlib.colors import Normalize
    from matplotlib.lines import Line2D
    from matplotlib.patches import Rectangle
    from matplotlib.ticker import FuncFormatter, MaxNLocator

    hour, path, background = job
    coast, opts = _worker
    with np.load(path, allow_pickle=False) as packed:
        data = {k: packed[k] for k in FIELDS}
    lon, lat, tec = (data[k] for k in ("ipp_longitude_deg", "ipp_latitude_deg", "stec_absolute_tecu"))
    valid = np.isfinite(tec) & np.isfinite(lon) & np.isfinite(lat)
    norm = Normalize(opts["vmin"], opts["vmax"], clip=False)
    cmap = matplotlib.colormaps["viridis"].with_extremes(under="#d81b60", over="#111111")
    fig, ax = plt.subplots(figsize=(12, 8), dpi=150, layout="constrained")
    fig.get_layout_engine().set(rect=(0, 0.065, 1, 0.935))
    try:
        ax.set_facecolor("white")
        if opts["sbas_requested"]:
            patches = [
                Rectangle((float(wrap(r["longitude"], opts["station"][0])) - 2.5, r["latitude"] - 2.5), 5, 5) for r in background
            ]
            bg_norm = Normalize(0, opts["sbas_vmax"])
            bg_cmap = matplotlib.colormaps["turbo"]
            cells = PatchCollection(patches, cmap=bg_cmap, norm=bg_norm, alpha=opts["background_alpha"], edgecolor="none", zorder=1)
            cells.set_array([r["vtec_tecu"] for r in background])
            ax.add_collection(cells)
            fig.colorbar(
                ScalarMappable(norm=bg_norm, cmap=bg_cmap),
                ax=ax,
                shrink=0.72,
                pad=0.025,
                label=f"SBAS PRN {opts['sbas_prn']} mean VTEC (TECU)\npale background",
                extend="max",
            )
        ax.add_collection(LineCollection(coast, colors="black", linewidths=0.55, zorder=2))
        endpoints = {}
        for indices in track_pieces(data, opts["join_gap_ns"]):
            xy = np.column_stack((lon[indices], lat[indices]))
            if len(indices) > 1:
                segments = np.stack((xy[:-1], xy[1:]), axis=1)
                ax.add_collection(LineCollection(segments, colors="white", linewidths=3.8, zorder=3))
                lines = LineCollection(segments, cmap=cmap, norm=norm, linewidths=2.3, zorder=4)
                lines.set_array((tec[indices[:-1]] + tec[indices[1:]]) / 2)
                ax.add_collection(lines)
            ax.scatter(xy[:, 0], xy[:, 1], c=tec[indices], cmap=cmap, norm=norm, s=5, zorder=5, linewidths=0)
            for index, marker in ((indices[0], "o"), (indices[-1], "^")):
                ax.scatter(
                    lon[index],
                    lat[index],
                    c=[tec[index]],
                    cmap=cmap,
                    norm=norm,
                    s=30,
                    marker=marker,
                    edgecolors="white",
                    linewidths=0.6,
                    zorder=6,
                )
            prn = int(data["prn"][indices[-1]])
            if prn not in endpoints or data["gpst_ns"][indices[-1]] > data["gpst_ns"][endpoints[prn]]:
                endpoints[prn] = indices[-1]
        for prn, index in endpoints.items():
            ax.annotate(
                f"G{prn:02d}",
                (lon[index], lat[index]),
                xytext=(5, 5),
                textcoords="offset points",
                fontsize=8,
                zorder=7,
                bbox=dict(facecolor="white", alpha=0.8, edgecolor="none", pad=1),
            )
        ax.scatter(*opts["station"], marker="*", s=130, c="black", edgecolors="white", linewidths=0.7, zorder=8)
        heading = calendar(hour).strftime("%Y-%m-%d %H:00 GPST")
        subtitle = f"GIM-constrained absolute STEC | {opts['signal_pair']} | {valid.sum():,}/{len(tec):,} usable samples"
        ax.set(
            title=f"{opts['station_name']} — GPS ionospheric pierce-point tracks\n{heading}\n{subtitle}",
            xlim=opts["extent"][:2],
            ylim=opts["extent"][2:],
            xlabel="Longitude",
            ylabel="Latitude",
        )
        ax.set_aspect("equal", adjustable="box")
        ax.xaxis.set_major_locator(MaxNLocator(nbins=8, steps=[1, 2, 2.5, 5, 10]))
        ax.yaxis.set_major_locator(MaxNLocator(nbins=8, steps=[1, 2, 2.5, 5, 10]))
        ax.xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{float(wrap(value,0)):g}°"))
        ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}°"))
        ax.grid(color="0.85", linewidth=0.5, zorder=0)
        fig.colorbar(
            ScalarMappable(norm=norm, cmap=cmap),
            ax=ax,
            shrink=0.85,
            pad=0.025,
            label="Absolute STEC (TECU)\nline of sight",
            extend="both",
        )
        ax.legend(
            handles=[
                Line2D([], [], color="black", marker="*", linestyle="none", label="Receiver"),
                Line2D([], [], color="0.4", marker="o", linestyle="none", label="Track start"),
                Line2D([], [], color="0.4", marker="^", linestyle="none", label="Track end"),
            ],
            loc="lower left",
            fontsize=8,
        )
        if not np.any(valid):
            ax.text(
                0.5,
                0.5,
                "Absolute STEC unavailable\nNo usable leveling / receiver DCB calibration",
                transform=ax.transAxes,
                ha="center",
                va="center",
                color="0.35",
                fontsize=13,
                bbox=dict(facecolor="white", alpha=0.9, edgecolor="none"),
            )
        bg_status = (
            f"SBAS: {len(background)} cells"
            if background
            else ("SBAS: unavailable" if opts["sbas_requested"] else "No SBAS background")
        )
        outside = int(np.sum(valid & ((tec < opts["vmin"]) | (tec > opts["vmax"]))))
        fig.text(
            0.5,
            0.015,
            f"450 km IPP shell; no hourly rebasing or gap filling • {bg_status} • {outside} samples outside colour scale\n"
            "STEC and SBAS VTEC are different quantities. Made with Natural Earth.",
            fontsize=8,
            ha="center",
            va="bottom",
        )
        target = Path(opts["output"]) / "png" / f"{label(hour)}.png"
        fig.savefig(
            target,
            facecolor="white",
            pil_kwargs={"compress_level": opts["png_compression"]},
            metadata={
                "Title": f"Absolute STEC {heading}",
                "Description": "GIM-constrained estimate; no independent absolute calibration. Made with Natural Earth.",
            },
        )
        return dict(
            path=str(target.relative_to(opts["output"])),
            hour_gpst=hour,
            samples=len(tec),
            valid_samples=int(valid.sum()),
            satellites=len(endpoints),
            sbas_cells=len(background),
            outside_scale=outside,
        )
    finally:
        plt.close(fig)


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option(
    "--coastline",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    required=True,
    help="Natural Earth coastline ZIP, preferably 10m.",
)
@click.option("--start", callback=parse_hour, help="First GPST hour, YYYY-MM-DDTHH.")
@click.option("--end", callback=parse_hour, help="Exclusive final GPST hour, YYYY-MM-DDTHH.")
@click.option(
    "--sbas-grid", type=click.Path(exists=True, file_okay=False, path_type=Path), help="Optional daily SBAS grid Parquet directory."
)
@click.option("--sbas-prn", type=click.IntRange(120, 158), default=137, show_default=True)
@click.option("--min-coverage", type=click.FloatRange(0, 1), default=0.25, show_default=True)
@click.option("--background-alpha", type=click.FloatRange(0, 1), default=0.18, show_default=True)
@click.option("--sbas-vmax", type=float, default=200, show_default=True)
@click.option("--vmin", type=float, default=0, show_default=True)
@click.option("--vmax", type=float, default=200, show_default=True)
@click.option("--extent", nargs=4, type=float, help="Fixed WEST EAST SOUTH NORTH; default shared bounds across selected hours.")
@click.option("--workers", type=click.IntRange(1, 32), default=min(4, os.cpu_count() or 1), show_default=True)
@click.option("--png-compression", type=click.IntRange(0, 9), default=3, show_default=True)
@click.option("--overwrite", is_flag=True)
@staged_output
def cli(
    input_dir,
    output,
    coastline,
    start,
    end,
    sbas_grid,
    sbas_prn,
    min_coverage,
    background_alpha,
    sbas_vmax,
    vmin,
    vmax,
    extent,
    workers,
    png_compression,
):
    """Render hourly absolute STEC trajectories using only finalized Parquet."""
    try:
        if start is not None and end is not None and end <= start:
            raise ValueError("--end must exceed --start")
        if not np.isfinite([vmin, vmax, sbas_vmax]).all() or vmax <= vmin or sbas_vmax <= 0:
            raise ValueError("Require finite vmin < vmax and positive sbas-vmax")
        paths = sorted((input_dir / "samples").glob("*.parquet"))
        if not paths:
            raise ValueError("No STEC samples")
        metadata = json.loads(pq.ParquetFile(paths[0]).schema_arrow.metadata[b"ngo"])
        if metadata.get("time_scale") != "GPST" or metadata.get("schema_version") != 1 or metadata.get("constellation") != "GPS":
            raise ValueError("Expected current GPS STEC Parquet")
        station = station_position(metadata["settings"]["position_ecef_m"])
        with tempfile.TemporaryDirectory(prefix="ngo-stec-plot-") as temporary:
            hours, bounds = prepare_hours(
                input_dir,
                Path(temporary),
                None if start is None else start * 10**9,
                None if end is None else end * 10**9,
                station[0],
            )
            if extent is None:
                bounds = [
                    min(bounds[0], station[0]),
                    max(bounds[1], station[0]),
                    min(bounds[2], station[1]),
                    max(bounds[3], station[1]),
                ]
                extent = (
                    max(station[0] - 180, math.floor((bounds[0] - 3) / 5) * 5),
                    min(station[0] + 180, math.ceil((bounds[1] + 3) / 5) * 5),
                    max(-90, math.floor((bounds[2] - 3) / 5) * 5),
                    min(90, math.ceil((bounds[3] + 3) / 5) * 5),
                )
            if not np.isfinite(extent).all() or not (0 < extent[1] - extent[0] <= 360 and -90 <= extent[2] < extent[3] <= 90):
                raise ValueError("Invalid map extent")
            background = Background(sbas_grid, [h for h, _ in hours], sbas_prn, min_coverage)
            output.mkdir()
            (output / "png").mkdir()
            options = dict(
                output=str(output),
                extent=extent,
                station=station,
                station_name=metadata["station"],
                signal_pair=f"C{metadata['signal1']}/C{metadata['signal2']}",
                vmin=vmin,
                vmax=vmax,
                sbas_vmax=sbas_vmax,
                sbas_prn=sbas_prn,
                sbas_requested=sbas_grid is not None,
                background_alpha=background_alpha,
                png_compression=png_compression,
                join_gap_ns=round(max(metadata["settings"]["gap_timeout"], 1.5 * metadata["settings"]["interval"]) * 10**9),
            )
            images = []
            jobs = iter(hours)
            with ProcessPoolExecutor(
                max_workers=min(workers, len(hours)),
                mp_context=multiprocessing.get_context("spawn"),
                initializer=initialize,
                initargs=(coastline, options),
            ) as pool:
                pending = set()
                with tqdm(total=len(hours), desc="Render absolute STEC", unit="image") as progress:
                    while True:
                        while len(pending) < 2 * workers:
                            entry = next(jobs, None)
                            if entry is None:
                                break
                            hour, path = entry
                            pending.add(pool.submit(render_hour, (hour, path, background.get(hour))))
                        if not pending:
                            break
                        done, pending = wait(pending, return_when=FIRST_COMPLETED)
                        for future in done:
                            images.append(future.result())
                            progress.update(1)
            images.sort(key=lambda r: r["hour_gpst"])
            policy = {k: v for k, v in options.items() if k != "output"}
            policy.update(
                time_scale="GPST",
                min_sbas_coverage=min_coverage,
                colour_quantity="absolute STEC, not VTEC",
                calibration="GIM-constrained",
                workers=workers,
            )
            write_json(output / "images.json", dict(time_scale="GPST", images=images, settings=policy))
            if sbas_grid is not None and not any(r["sbas_cells"] for r in images):
                click.echo("No matching SBAS cells; rendered without background.", err=True)
            click.echo(
                json.dumps(dict(status="complete", images=len(images), valid_samples=sum(r["valid_samples"] for r in images)))
            )
    except (ValueError, KeyError, OSError, RuntimeError, zipfile.BadZipFile) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
