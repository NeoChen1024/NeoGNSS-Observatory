# SPDX-License-Identifier: GPL-3.0-only
"""Experimental arc-relative dSTEC and hourly ionospheric pierce-point maps."""

import csv
import json
import math
import multiprocessing
import os
import subprocess
import tempfile
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import click
from tqdm import tqdm

from .gpst import label as gpst_label
from .research_output import staged_output

LIGHT_SPEED = 299792458.0
os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / f"neognss-matplotlib-{os.getuid()}"))


def phase_gf(phase1, phase2, frequency1, frequency2):
    return LIGHT_SPEED * (phase1 / frequency1 - phase2 / frequency2)


class ArcTracker:
    def __init__(self, gap_seconds=1.5, jump_m=0.1):
        self.gap_seconds, self.jump_m = gap_seconds, jump_m
        self.states = {}
        self.counts = Counter()
        self.diagnostics = Counter()

    def process(self, row, minimum_elevation):
        key = (row["satellite"], row["code1"], row["code2"])
        t = float(row["gpst"])
        f1, f2 = float(row["frequency1"]), float(row["frequency2"])
        l1, l2 = float(row["phase1"]), float(row["phase2"])
        lli = int(row["lli1"]) | int(row["lli2"])
        reason = None
        if not all(math.isfinite(v) for v in (t, f1, f2, l1, l2)) or f1 <= 0 or f2 <= 0 or f1 == f2 or not l1 or not l2:
            reason = "missing_phase_or_frequency"
        elif lli & 2:
            reason = "unresolved_half_cycle"
        elif not int(row["geometry_ok"]):
            reason = "unavailable_or_unhealthy_orbit"
        elif float(row["elevation"]) < minimum_elevation:
            reason = "low_elevation"
        if reason:
            self.states.pop(key, None)
            self.diagnostics[reason] += 1
            return None
        gf = phase_gf(l1, l2, f1, f2)
        previous = self.states.get(key)
        reset = "first_valid"
        if previous is not None:
            last, last_gf, reference, previous_frequencies = previous
            if t <= last:
                raise ValueError(f"Non-increasing observation time for {key}: {t}")
            if t - last > self.gap_seconds:
                reset = "epoch_gap"
            elif (f1, f2) != previous_frequencies:
                reset = "frequency_change"
            elif lli & 1:
                reset = "lli_slip"
            elif abs(gf - last_gf) > self.jump_m:
                reset = "gf_jump_candidate"
            else:
                reset = None
        if reset:
            reference = gf
            self.counts[key] += 1
            self.diagnostics[reset] += 1
        self.states[key] = (t, gf, reference, (f1, f2))
        coefficient = 40.3e16 * (1 / f2**2 - 1 / f1**2)
        return {
            "gpst": t,
            "gps_week": int(row["gps_week"]),
            "gps_tow": float(row["gps_tow"]),
            "satellite": key[0],
            "signal_pair": f"L{key[1]}-L{key[2]}",
            "arc_id": f"{key[0]}-{key[1]}-{key[2]}-{self.counts[key]}",
            "arc_start_reason": reset,
            "gf_m": gf,
            "dstec_tecu": (gf - reference) / coefficient,
            "latitude": float(row["ipp_latitude"]),
            "longitude": float(row["ipp_longitude"]),
            "azimuth": float(row["azimuth"]),
            "elevation": float(row["elevation"]),
            "mapping": float(row["mapping"]),
            "lli1": int(row["lli1"]),
            "lli2": int(row["lli2"]),
        }


def header_position(path):
    with path.open() as stream:
        for line in stream:
            if "APPROX POSITION XYZ" in line[60:]:
                return tuple(float(v) for v in line[:60].split())
            if "END OF HEADER" in line:
                break
    raise ValueError(f"RINEX lacks an approximate station position: {path}")


def station_lonlat(position):
    x, y, z = position
    radius = math.hypot(x, y)
    e2 = 6.6943799901413165e-3
    lat = math.atan2(z, radius * (1 - e2))
    for _ in range(10):
        n = 6378137.0 / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        lat = math.atan2(z + e2 * n * math.sin(lat), radius)
    return math.degrees(math.atan2(y, x)), math.degrees(lat)


def process_group(job):
    number, obs, nav, worker, directory, station, shell, elevation, gap, jump = job
    directory = Path(directory)
    csv_path = directory / f"group-{number:05d}.csv"
    tracks = directory / f"group-{number:05d}.jsonl"
    station_source = "explicit_ecef" if station is not None else "rinex_approximate_position"
    position = station or header_position(obs)
    if len(position) != 3 or not all(math.isfinite(v) for v in position):
        raise ValueError("Station position must contain three finite ECEF coordinates")
    command = [str(worker), str(obs), str(nav), *map(str, position), str(shell), str(csv_path)]
    with (directory / f"group-{number:05d}.log").open("x") as log:
        subprocess.run(command, check=True, stderr=log)
    tracker = ArcTracker(gap, jump)
    counts = Counter()
    first_gpst = last_gpst = None
    with csv_path.open() as stream, tracks.open("x") as output:
        for row in csv.DictReader(stream):
            counts["input_rows"] += 1
            t = float(row["gpst"])
            first_gpst = t if first_gpst is None else min(first_gpst, t)
            last_gpst = t if last_gpst is None else max(last_gpst, t)
            point = tracker.process(row, elevation)
            if point is None:
                continue
            point["arc_id"] = f"group-{number:05d}-" + point["arc_id"]
            output.write(json.dumps(point, separators=(",", ":")) + "\n")
            counts["valid_points"] += 1
    return dict(
        obs=str(obs),
        nav=str(nav),
        command=command,
        station_ecef=position,
        station_source=station_source,
        geometry_csv=str(csv_path),
        tracks=str(tracks),
        counts=dict(counts),
        diagnostics=dict(tracker.diagnostics),
        arcs=sum(tracker.counts.values()),
        first_gpst=first_gpst,
        last_gpst=last_gpst,
    )


def hourly_display_arcs(points):
    """Rebase one hour's display without mutating continuous-arc observations."""
    arcs = defaultdict(list)
    for point in sorted(points, key=lambda p: p["gpst"]):
        arcs[point["arc_id"]].append(point)
    references = {}
    for key, arc in arcs.items():
        baseline = arc[0]["dstec_tecu"]
        references[key] = dict(gpst=arc[0]["gpst"], dstec_tecu=baseline)
        arcs[key] = [dict(point, display_dstec_tecu=point["dstec_tecu"] - baseline) for point in arc]
    return arcs, references


def plot_hour(job):
    # Each process owns its Matplotlib figures and PNG compressor.
    import matplotlib.pyplot as plt
    import numpy as np
    from matplotlib import colormaps
    from matplotlib.cm import ScalarMappable
    from matplotlib.collections import LineCollection, PatchCollection
    from matplotlib.colors import ListedColormap, Normalize
    from matplotlib.patches import Rectangle

    from .sbas_grid_render import coastline_parts

    hour, points, cells, options = job
    coast = list(coastline_parts(options["coastline"]))
    fig, ax = plt.subplots(figsize=(12, 8), dpi=150, constrained_layout=True)
    ax.set_facecolor("white")
    # Blend the colormap itself, so the colorbar matches the pale background.
    colors = colormaps["turbo"](np.linspace(0, 1, 256))
    colors[:, :3] = 1 - options["background_alpha"] * (1 - colors[:, :3])
    pale = ListedColormap(colors)
    background_norm = Normalize(0, 100, clip=True)
    if cells:
        patches = [Rectangle((c["longitude"] - 2.5, c["latitude"] - 2.5), 5, 5) for c in cells]
        collection = PatchCollection(patches, cmap=pale, norm=background_norm, edgecolor="none", zorder=1)
        collection.set_array([c["vtec_tecu"] for c in cells])
        ax.add_collection(collection)
        fig.colorbar(
            ScalarMappable(norm=background_norm, cmap=pale),
            ax=ax,
            shrink=0.72,
            pad=0.02,
            label=f"SBAS PRN {options['sbas_prn']} hourly VTEC (TECU)",
        )
    ax.add_collection(LineCollection(coast, colors="0.3", linewidths=0.65, zorder=2))
    norm = Normalize(-options["color_limit"], options["color_limit"], clip=True)
    arcs, references = hourly_display_arcs(points)
    labels = {}
    for arc in arcs.values():
        xy = np.array([(p["longitude"], p["latitude"]) for p in arc])
        values = np.array([p["display_dstec_tecu"] for p in arc])
        if len(arc) > 1:
            pairs = np.stack((xy[:-1], xy[1:]), axis=1)
            valid = np.abs(xy[1:, 0] - xy[:-1, 0]) < 180
            line = LineCollection(pairs[valid], cmap="coolwarm", norm=norm, linewidths=2.2, zorder=4)
            line.set_array((values[:-1] + values[1:])[valid] / 2)
            ax.add_collection(line)
            if valid[-1]:
                ax.annotate("", xy=xy[-1], xytext=xy[-2], arrowprops=dict(arrowstyle="->", color="0.15", lw=0.8), zorder=5)
        ax.scatter(xy[0, 0], xy[0, 1], c=[values[0]], cmap="coolwarm", norm=norm, s=9, zorder=4)
        labels[arc[-1]["satellite"]] = xy[-1]
    for satellite, xy in sorted(labels.items()):
        ax.annotate(satellite, xy, xytext=(3, 3), textcoords="offset points", fontsize=7, zorder=6)
    lon, lat = options["station_lonlat"]
    ax.plot(lon, lat, marker="*", color="black", markersize=10, zorder=7)
    extent = options["extent"]
    label = gpst_label(hour)
    ax.set(
        xlim=extent[:2],
        ylim=extent[2:],
        xlabel="Longitude",
        ylabel="Latitude",
        title=f"IPP tracks — {label} (one hour)\n{options['shell_km']:g} km shell; hour-relative dSTEC per arc; "
        + ("pale SBAS hourly background" if cells else "SBAS background unavailable"),
    )
    ax.set_aspect("equal", adjustable="box")
    ax.grid(color="0.8", linewidth=0.5, zorder=0)
    fig.colorbar(
        ScalarMappable(norm=norm, cmap="coolwarm"),
        ax=ax,
        shrink=0.72,
        pad=0.02,
        extend="both",
        label="dSTEC from first sample of each arc in this hour (TECU)",
    )
    path = Path(options["output"]) / "png" / (label.replace(":", "-") + ".png")
    fig.savefig(path, facecolor="white", pil_kwargs={"compress_level": options["png_compression"]})
    plt.close(fig)
    return dict(
        path=str(path.relative_to(options["output"])),
        hour_gpst=hour,
        points=len(points),
        arcs=len(arcs),
        display_references=references,
        sbas_cells=len(cells),
    )


def parallel_map(function, jobs, workers, description):
    if workers == 1 or len(jobs) <= 1:
        return [function(job) for job in tqdm(jobs, desc=description)]
    with ProcessPoolExecutor(max_workers=min(workers, len(jobs)), mp_context=multiprocessing.get_context("spawn")) as pool:
        return list(tqdm(pool.map(function, jobs), total=len(jobs), desc=description))


@click.command()
@click.option(
    "--obs",
    multiple=True,
    required=True,
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    help="One RINEX OBS per independent continuous group; repeat for parallel groups.",
)
@click.option("--nav", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--geometry-worker", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--sbas-hourly", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--coastline", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--output", required=True, type=click.Path(path_type=Path))
@click.option("--station-ecef", nargs=3, type=float, help="Station ECEF XYZ in metres; default: RINEX approximate position.")
@click.option("--workers", type=click.IntRange(1, 32), default=min(4, os.cpu_count() or 1), show_default=True)
@click.option("--shell-km", type=click.FloatRange(100, 1000), default=350.0, show_default=True)
@click.option("--minimum-elevation", type=click.FloatRange(0, 90), default=20.0, show_default=True)
@click.option("--gap-seconds", type=click.FloatRange(min=0, min_open=True), default=1.5, show_default=True)
@click.option("--jump-m", type=click.FloatRange(min=0, min_open=True), default=0.1, show_default=True)
@click.option("--plot-step", type=click.FloatRange(min=0, min_open=True), default=10.0, show_default=True)
@click.option("--color-limit", type=click.FloatRange(min=0, min_open=True), default=50.0, show_default=True)
@click.option("--background-alpha", type=click.FloatRange(0, 1), default=0.18, show_default=True)
@click.option("--sbas-prn", type=int, default=137, show_default=True)
@click.option("--png-compression", type=click.IntRange(0, 9), default=3, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(
    obs,
    nav,
    geometry_worker,
    sbas_hourly,
    coastline,
    output,
    station_ecef,
    workers,
    shell_km,
    minimum_elevation,
    gap_seconds,
    jump_m,
    plot_step,
    color_limit,
    background_alpha,
    sbas_prn,
    png_compression,
):
    """Render hourly IPP trajectories over pale SBAS VTEC grids."""
    try:
        if len(obs) > 1 and station_ecef is None:
            raise ValueError("Multiple groups require a shared explicit --station-ecef position")
        if len({p.resolve() for p in obs}) != len(obs):
            raise ValueError("The same OBS file was supplied more than once")
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        (output / "png").mkdir()
        directory = output / "tracks"
        directory.mkdir()
        jobs = [
            (
                i,
                p.resolve(),
                nav.resolve(),
                geometry_worker.resolve(),
                directory,
                station_ecef,
                shell_km,
                minimum_elevation,
                gap_seconds,
                jump_m,
            )
            for i, p in enumerate(obs)
        ]
        groups = parallel_map(process_group, jobs, workers, "RINEX geometry + dSTEC")
        timed_groups = sorted((g for g in groups if g["first_gpst"] is not None), key=lambda g: g["first_gpst"])
        for previous, incoming in zip(timed_groups, timed_groups[1:]):
            if incoming["first_gpst"] <= previous["last_gpst"]:
                raise ValueError("Independent OBS groups overlap in time; resolve duplicate coverage before mapping")
        hourly = defaultdict(list)
        last_plot = {}
        # Keep endpoints for every arc/hour; never join separate arcs.
        pending = {}
        for group in groups:
            with Path(group["tracks"]).open() as stream:
                for line in stream:
                    point = json.loads(line)
                    hour = math.floor(point["gpst"] / 3600) * 3600
                    key = (hour, point["arc_id"])
                    if point["gpst"] - last_plot.get(key, -math.inf) >= plot_step:
                        hourly[hour].append(point)
                        last_plot[key] = point["gpst"]
                    pending[key] = point
        for (hour, arc), point in pending.items():
            if last_plot[hour, arc] != point["gpst"]:
                hourly[hour].append(point)
        if not hourly:
            raise ValueError("No valid dual-frequency observations with usable geometry")
        background = defaultdict(list)
        with sbas_hourly.open() as stream:
            for line in stream:
                row = json.loads(line)
                if (
                    row["hour_gpst"] in hourly
                    and row["prn"] == sbas_prn
                    and row["coverage"] >= 0.25
                    and (row["constellation"], row["signal"]) == ("SBAS", "L1CA")
                ):
                    background[row["hour_gpst"]].append(row)
        lon, lat = station_lonlat(groups[0]["station_ecef"])
        all_points = [p for rows in hourly.values() for p in rows]
        lons = [p["longitude"] for p in all_points] + [lon]
        lats = [p["latitude"] for p in all_points] + [lat]
        extent = [
            max(-180, math.floor(min(lons) / 5) * 5 - 5),
            min(180, math.ceil(max(lons) / 5) * 5 + 5),
            max(-90, math.floor(min(lats) / 5) * 5 - 5),
            min(90, math.ceil(max(lats) / 5) * 5 + 5),
        ]
        options = dict(
            output=str(output),
            coastline=str(coastline.resolve()),
            background_alpha=background_alpha,
            color_limit=color_limit,
            sbas_prn=sbas_prn,
            shell_km=shell_km,
            extent=extent,
            station_lonlat=(lon, lat),
            png_compression=png_compression,
        )
        render_jobs = [
            (hour, sorted(points, key=lambda p: p["gpst"]), background[hour], options) for hour, points in sorted(hourly.items())
        ]
        images = parallel_map(plot_hour, render_jobs, workers, "Render hourly IPP PNG")
        (output / "images.json").write_text(json.dumps(dict(time_scale="GPST", images=images), indent=2) + "\n")
        policy = dict(
            options,
            minimum_elevation=minimum_elevation,
            gap_seconds=gap_seconds,
            jump_m=jump_m,
            plot_step_seconds=plot_step,
            workers=workers,
            reference="first valid phase GF in each arc",
            display_reference="first valid sample of each arc within each GPST hour",
            orbit="broadcast; no precise fallback",
            sbas_minimum_coverage=0.25,
            limitations=[
                "C59 excluded by pinned RTKLIB",
                "fixed experimental signal pairs",
                "GF jump is a candidate detector, not a complete cycle-slip solution",
                "dSTEC is not absolute VTEC; no spatial interpolation",
            ],
        )
        report = dict(
            status="complete",
            time_scale="GPST",
            time_origin="1980-01-06 00:00:00 GPST",
            groups=groups,
            policy=policy,
            images=len(images),
        )
        policy.pop("output")
        for group in groups:
            group.pop("command", None)
            for key in ("geometry_csv", "tracks"):
                group[key] = str(Path(group[key]).relative_to(output))
        (output / "completed.json").write_text(json.dumps(report, indent=2) + "\n")
        click.echo(
            json.dumps(
                dict(
                    status="complete",
                    images=len(images),
                    valid_points=sum(g["counts"].get("valid_points", 0) for g in groups),
                )
            )
        )
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
