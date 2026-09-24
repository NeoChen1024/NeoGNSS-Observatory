# SPDX-License-Identifier: GPL-3.0-only
"""Rendering helpers for protocol-neutral hourly SBAS cells."""

import multiprocessing
import os
import shutil
import tempfile
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import click

from .gpst import label as gpst_label
from .gpst import parse_hour as parse_gpst_hour
from .map_assets import coastline_parts

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / f"neognss-matplotlib-{os.getuid()}"))
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.cm import ScalarMappable
from matplotlib.collections import LineCollection, PatchCollection
from matplotlib.colors import Normalize
from matplotlib.patches import Rectangle
from matplotlib.ticker import FuncFormatter
from tqdm import tqdm

_coast_key = None
_coast = None


def initialize_coastline(path, extent):
    global _coast_key, _coast
    key = (str(Path(path).resolve()), tuple(extent))
    if key == _coast_key:
        return
    west, east, south, north = extent
    # Keep an outside margin for strokes/antialiasing clipped at the axes edge.
    west, east, south, north = west - 1, east + 1, south - 1, north + 1
    segments = []
    for part in coastline_parts(path):
        vertices = np.asarray(part, dtype=float)
        if len(vertices) < 2:
            continue
        center = (extent[0] + extent[1]) / 2
        vertices[:, 0] = (vertices[:, 0] - center + 180) % 360 - 180 + center
        for segment in np.split(vertices, np.flatnonzero(np.abs(np.diff(vertices[:, 0])) > 180) + 1):
            if len(segment) < 2 or segment[:, 0].max() < west or segment[:, 0].min() > east:
                continue
            if segment[:, 1].max() < south or segment[:, 1].min() > north:
                continue
            segment.flags.writeable = False
            segments.append(segment)
    _coast_key, _coast = key, segments


def extent_for(rows):
    lons, lats = [row["longitude"] for row in rows], [row["latitude"] for row in rows]
    # Cut at the largest empty longitude gap, keeping Pacific coverage together.
    values = np.unique(np.asarray(lons) % 360)
    gap = np.argmax(np.diff(np.r_[values, values[0] + 360]))
    origin = values[(gap + 1) % len(values)]
    lons = (np.asarray(lons) - origin) % 360 + origin
    west, east = (min(lons) - 10) // 10 * 10, (max(lons) + 19) // 10 * 10
    if east - west > 360:
        west, east = -180, 180
    return (
        west,
        east,
        max(-90, (min(lats) - 10) // 10 * 10),
        min(90, (max(lats) + 19) // 10 * 10),
    )


def render_serial(rows, coastline, output, vmin, vmax, min_coverage, extent=None, show_progress=True, png_compression=3):
    selected = [row for row in rows if row["coverage"] >= min_coverage]
    if not selected:
        raise ValueError("No hourly grid cells meet the coverage threshold")
    extent = extent if extent is not None else extent_for(selected)
    aspect = (extent[1] - extent[0]) / (extent[3] - extent[2])
    # Size the canvas around the equal-aspect map, not an arbitrary wide page.
    # A shared extent keeps all frames in a rendering batch the same size.
    figsize = (max(8.8, 7.0 * aspect + 1.8), 8)
    initialize_coastline(coastline, extent)
    coast = _coast
    grouped = defaultdict(list)
    for row in selected:
        grouped[row["hour_gpst"]].append(row)
    image_dir = output / "png"
    image_dir.mkdir()
    manifest = []
    norm = Normalize(vmin=vmin, vmax=vmax, clip=True)
    cmap = matplotlib.colormaps["turbo"]
    for key, cells in tqdm(sorted(grouped.items()), desc="Render hourly PNG", unit="image", disable=not show_progress):
        hour = key
        sources = sorted({v["provider"] for cell in cells for v in cell["sources"]})
        mt0 = any(v["mt0_seconds"] > 0 for cell in cells for v in cell["sources"])
        directory = image_dir
        label = gpst_label(hour)
        path = directory / (label.replace(":", "-") + ".png")
        fig, ax = plt.subplots(figsize=figsize, dpi=150, constrained_layout=True)
        ax.set_facecolor("white")
        center = (extent[0] + extent[1]) / 2
        patches = [
            Rectangle(((cell["longitude"] - center + 180) % 360 - 180 + center - 2.5, cell["latitude"] - 2.5), 5, 5)
            for cell in cells
        ]
        collection = PatchCollection(patches, cmap=cmap, norm=norm, edgecolor=(0, 0, 0, 0.22), linewidth=0.25, zorder=2)
        collection.set_array([cell["vtec_tecu"] for cell in cells])
        ax.add_collection(collection)
        ax.add_collection(LineCollection(coast, colors="black", linewidths=0.7, zorder=3))
        ax.set(
            xlim=extent[:2],
            ylim=extent[2:],
            xlabel="Longitude",
            ylabel="Latitude",
            title=f"SBAS composite hourly mean VTEC — {label}\n"
            f"{' / '.join(sources)} | coverage ≥ {min_coverage:.0%}" + (" | includes MT0 research data" if mt0 else ""),
        )
        ax.set_aspect("equal", adjustable="box")
        ax.set_xticks(range(int(extent[0]), int(extent[1]) + 1, 10))
        ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{(x + 180) % 360 - 180:g}°"))
        ax.set_yticks(range(int(extent[2]), int(extent[3]) + 1, 10))
        ax.grid(color="0.75", linewidth=0.5, zorder=1)
        fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax, label="VTEC (TECU)", shrink=0.82)
        fig.savefig(
            path,
            facecolor="white",
            metadata={
                "Title": f"SBAS composite hourly mean VTEC {label}",
                "Description": "Experimental time-weighted MT26 grid; Made with Natural Earth.",
            },
            pil_kwargs={"compress_level": png_compression},
        )
        plt.close(fig)
        manifest.append(
            dict(
                path=str(path.relative_to(output)),
                hour_gpst=hour,
                providers=sources,
                includes_mt0=mt0,
                cells=len(cells),
            )
        )
    return manifest, extent


def render_job(job):
    rows, coastline, output, vmin, vmax, min_coverage, extent, compression = job
    # Each worker owns a temporary output tree and publishes its unique hour.
    with tempfile.TemporaryDirectory(prefix=".render-", dir=output) as temporary:
        manifest, _ = render_serial(rows, coastline, Path(temporary), vmin, vmax, min_coverage, extent, False, compression)
        for record in manifest:
            target = output / record["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(Path(temporary) / record["path"]), target)
    return manifest


def render(rows, coastline, output, vmin, vmax, min_coverage, workers=4, png_compression=3):
    selected = [r for r in rows if r["coverage"] >= min_coverage]
    if not selected:
        raise ValueError("No hourly grid cells meet the coverage threshold")
    extent = extent_for(selected)
    groups = defaultdict(list)
    for row in selected:
        key = row["hour_gpst"]
        groups[key].append(row)
    if workers == 1 or len(groups) == 1:
        return render_serial(selected, coastline, output, vmin, vmax, min_coverage, extent, True, png_compression)
    jobs = [(cells, coastline, output, vmin, vmax, min_coverage, extent, png_compression) for _, cells in sorted(groups.items())]
    manifest = []
    with ProcessPoolExecutor(max_workers=min(workers, len(jobs)), mp_context=multiprocessing.get_context("spawn")) as pool:
        for result in tqdm(pool.map(render_job, jobs), total=len(jobs), desc="Render hourly PNG", unit="image"):
            manifest.extend(result)
    return manifest, extent


def parse_hour(_context, _parameter, value):
    if value is None:
        return None
    try:
        parsed = parse_gpst_hour(value)
    except ValueError as error:
        raise click.BadParameter("use YYYY-MM-DDTHH in GPST") from error
    return parsed
