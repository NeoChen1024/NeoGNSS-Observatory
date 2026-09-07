# SPDX-License-Identifier: GPL-3.0-only
"""Rendering helpers for protocol-neutral hourly SBAS cells."""

import multiprocessing
import os
import shutil
import tempfile
import zipfile
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from io import BytesIO
from pathlib import Path

import click

from .gpst import label as gpst_label
from .gpst import parse_hour as parse_gpst_hour
from .sbas_streams import IDENTITY

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / f"neognss-matplotlib-{os.getuid()}"))
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import shapefile
from matplotlib.cm import ScalarMappable
from matplotlib.collections import LineCollection, PatchCollection
from matplotlib.colors import Normalize
from matplotlib.patches import Rectangle
from tqdm import tqdm


def coastline_parts(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        shp = next(name for name in names if name.endswith(".shp"))
        shx = next(name for name in names if name.endswith(".shx"))
        dbf = next(name for name in names if name.endswith(".dbf"))
        reader = shapefile.Reader(shp=BytesIO(archive.read(shp)), shx=BytesIO(archive.read(shx)), dbf=BytesIO(archive.read(dbf)))
        for shape in reader.shapes():
            boundaries = list(shape.parts) + [len(shape.points)]
            for begin, finish in zip(boundaries, boundaries[1:]):
                yield shape.points[begin:finish]


def extent_for(rows):
    lons, lats = [row["longitude"] for row in rows], [row["latitude"] for row in rows]
    return (
        max(-180, (min(lons) - 10) // 10 * 10),
        min(180, (max(lons) + 19) // 10 * 10),
        max(-90, (min(lats) - 10) // 10 * 10),
        min(90, (max(lats) + 19) // 10 * 10),
    )


def render_serial(rows, coastline, output, vmin, vmax, min_coverage, extent=None, show_progress=True, png_compression=3):
    selected = [row for row in rows if row["coverage"] >= min_coverage]
    if not selected:
        raise ValueError("No hourly grid cells meet the coverage threshold")
    extent = extent if extent is not None else extent_for(selected)
    coast = list(coastline_parts(coastline))
    grouped = defaultdict(list)
    for row in selected:
        grouped[tuple(row[k] for k in (*IDENTITY, "hour_gpst"))].append(row)
    image_dir = output / "png"
    image_dir.mkdir()
    manifest = []
    norm = Normalize(vmin=vmin, vmax=vmax, clip=True)
    cmap = matplotlib.colormaps["turbo"]
    for key, cells in tqdm(sorted(grouped.items()), desc="Render hourly PNG", unit="image", disable=not show_progress):
        constellation, prn, signal, hour = key
        directory = image_dir / f"{constellation}_prn-{prn}_{signal}"
        directory.mkdir(exist_ok=True)
        label = gpst_label(hour)
        path = directory / (label.replace(":", "-") + ".png")
        fig, ax = plt.subplots(figsize=(12, 8), dpi=150, constrained_layout=True)
        ax.set_facecolor("white")
        patches = [Rectangle((cell["longitude"] - 2.5, cell["latitude"] - 2.5), 5, 5) for cell in cells]
        collection = PatchCollection(patches, cmap=cmap, norm=norm, edgecolor=(0, 0, 0, 0.22), linewidth=0.25, zorder=2)
        collection.set_array([cell["vtec_tecu"] for cell in cells])
        ax.add_collection(collection)
        ax.add_collection(LineCollection(coast, colors="black", linewidths=0.7, zorder=3))
        ax.set(
            xlim=extent[:2],
            ylim=extent[2:],
            xlabel="Longitude",
            ylabel="Latitude",
            title=f"SBAS PRN {prn} hourly mean VTEC — {label}\nvalid coverage ≥ {min_coverage:.0%}",
        )
        ax.set_aspect("equal", adjustable="box")
        ax.set_xticks(range(int(extent[0]), int(extent[1]) + 1, 10))
        ax.set_yticks(range(int(extent[2]), int(extent[3]) + 1, 10))
        ax.grid(color="0.75", linewidth=0.5, zorder=1)
        fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax, label="VTEC (TECU)", shrink=0.82)
        fig.savefig(
            path,
            facecolor="white",
            metadata={
                "Title": f"SBAS PRN {prn} hourly mean VTEC {label}",
                "Description": "Experimental time-weighted MT26 grid; Made with Natural Earth.",
            },
            pil_kwargs={"compress_level": png_compression},
        )
        plt.close(fig)
        manifest.append(
            dict(
                path=str(path.relative_to(output)),
                hour_gpst=hour,
                constellation=constellation,
                prn=prn,
                signal=signal,
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
        key = tuple(row[k] for k in (*IDENTITY, "hour_gpst"))
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
