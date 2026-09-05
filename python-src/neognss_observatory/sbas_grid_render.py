# SPDX-License-Identifier: GPL-3.0-only
"""Build experimental hourly SBAS VTEC maps from extracted Era A frames."""

import bisect
import hashlib
import importlib.metadata
import json
import mmap
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter, defaultdict
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path

import click

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

from .sbas_grid import HourlyGrid
from .ubx_restitch import RECORD, UNKNOWN

INDEX_HEADER_SIZE = 48


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


class EpochIndex:
    def __init__(self, path, expected_sha256):
        self.path = path
        if sha256(path) != expected_sha256:
            raise ValueError(f"Reconstruction epoch index checksum mismatch: {path}")
        self.stream = path.open("rb")
        self.data = mmap.mmap(self.stream.fileno(), 0, access=mmap.ACCESS_READ)
        if self.data[:8] != b"UBXIDX02" or (len(self.data) - INDEX_HEADER_SIZE) % RECORD.size:
            raise ValueError(f"Invalid reconstruction epoch index: {path}")
        self.count = (len(self.data) - INDEX_HEADER_SIZE) // RECORD.size

    def record(self, number):
        return RECORD.unpack_from(self.data, INDEX_HEADER_SIZE + number * RECORD.size)

    def locate(self, offset, hint=None):
        if hint is not None:
            begin, end, utc, *_ = self.record(hint)
            if begin <= offset < end:
                return hint, utc
            if offset >= end and hint + 1 < self.count:
                begin, end, utc, *_ = self.record(hint + 1)
                if begin <= offset < end:
                    return hint + 1, utc
        low, high = 0, self.count
        while low < high:
            middle = (low + high) // 2
            if self.record(middle)[0] <= offset:
                low = middle + 1
            else:
                high = middle
        number = low - 1
        if number < 0:
            raise ValueError(f"Offset precedes epoch index: {self.path}:{offset}")
        begin, end, utc, *_ = self.record(number)
        if not begin <= offset < end or utc == UNKNOWN:
            raise ValueError(f"SBAS offset has no payload-derived UTC epoch: {self.path}:{offset}")
        return number, utc

    def close(self):
        self.data.close()
        self.stream.close()


class EraATimeMapper:
    def __init__(self, reconstruction):
        self.reconstruction = reconstruction
        self.source_indexes = {}
        self.indexes = {}
        self.artifacts = {}
        with (reconstruction / "plan.jsonl").open() as stream:
            for line in stream:
                record = json.loads(line)
                if record.get("record_type") == "sources":
                    path = reconstruction / "provenance/indexes" / (record["sha256"] + ".idx")
                    self.source_indexes[record["path"]] = (path, record["index_sha256"])
                elif record.get("record_type") == "artifacts" and record.get("kind") == "utc_segment":
                    self.artifacts[record["name"]] = record

    def index(self, source):
        if source not in self.indexes:
            path, digest = self.source_indexes[source]
            self.indexes[source] = EpochIndex(path, digest)
        return self.indexes[source]

    def artifact_cursor(self, name):
        artifact = self.artifacts[name]
        ends, total = [], 0
        for span in artifact["spans"]:
            total += span["end"] - span["begin"]
            ends.append(total)
        if total != artifact["size"]:
            raise ValueError(f"Artifact span size mismatch: {name}")
        return ArtifactCursor(self, artifact, ends)

    def close(self):
        for index in self.indexes.values():
            index.close()


class ArtifactCursor:
    def __init__(self, mapper, artifact, ends):
        self.mapper, self.artifact, self.ends = mapper, artifact, ends
        self.hints = {}

    def utc(self, local_offset):
        if not 0 <= local_offset < self.artifact["size"]:
            raise ValueError(f"SBAS offset outside artifact: {self.artifact['name']}:{local_offset}")
        number = bisect.bisect_right(self.ends, local_offset)
        span = self.artifact["spans"][number]
        local_begin = 0 if number == 0 else self.ends[number - 1]
        original = span["begin"] + local_offset - local_begin
        index = self.mapper.index(span["source"])
        hint, utc = index.locate(original, self.hints.get(span["source"]))
        self.hints[span["source"]] = hint
        if not self.artifact["start_utc"] <= utc <= self.artifact["end_utc"]:
            raise ValueError(f"Mapped UTC outside artifact: {self.artifact['name']}")
        return utc


class GroupOffsetMapper:
    def __init__(self, mapper, sources):
        self.sources = sources
        self.ends = [source["stream_end"] for source in sources]
        self.cursors = [mapper.artifact_cursor(source["name"]) for source in sources]

    def utc(self, offset):
        number = bisect.bisect_right(self.ends, offset)
        if number == len(self.sources):
            raise ValueError(f"SBAS offset outside continuous group: {offset}")
        source = self.sources[number]
        if offset < source["stream_begin"]:
            raise ValueError(f"SBAS offset falls between group sources: {offset}")
        return self.cursors[number].utc(offset - source["stream_begin"])


def group_paths(extraction):
    with (extraction / "groups.jsonl").open() as stream:
        for line in stream:
            record = json.loads(line)
            yield extraction / record["group"]


def aggregate(extraction, reconstruction, start=None, end=None):
    completed = json.loads((extraction / "completed.json").read_text())
    if completed.get("status") != "complete":
        raise ValueError("SBAS extraction is not complete")
    paths = list(group_paths(extraction))
    if len(paths) != completed["continuous_groups"]:
        raise ValueError("SBAS group journal is incomplete")
    total = sum(path.stat().st_size for group in paths for path in group.glob("gnss-1_*.jsonl"))
    mapper = EraATimeMapper(reconstruction)
    combined = defaultdict(lambda: [0.0, 0])
    diagnostics = Counter()
    messages = Counter()
    try:
        with tqdm(total=total, desc="Aggregate SBAS grid", unit="B", unit_scale=True, mininterval=2) as progress:
            for group in paths:
                source_info = json.loads((group / "sources.json").read_text())
                if end is not None and source_info["start_utc"] >= end or start is not None and source_info["end_utc"] < start:
                    progress.update(sum(path.stat().st_size for path in group.glob("gnss-1_*.jsonl")))
                    continue
                for path in sorted(group.glob("gnss-1_*.jsonl")):
                    offset_mapper = GroupOffsetMapper(mapper, source_info["sources"])
                    states = {}
                    with path.open("rb") as stream:
                        for line in stream:
                            # MT0 can invalidate state; MT18/26 define and update the grid.
                            if not any(marker in line for marker in (b'"type":0,', b'"type":18,', b'"type":26,')):
                                continue
                            record = json.loads(line)
                            message = record["sbas"]
                            time = offset_mapper.utc(record["offset"])
                            key = (record["gnssId"], record["svId"], record["sigId"], record["freqId"])
                            state = states.setdefault(key, HourlyGrid())
                            state.process(time, message)
                            messages[str(message["type"])] += 1
                    progress.update(path.stat().st_size)
                    for key, state in states.items():
                        state.finish(source_info["end_utc"] + 1)
                        diagnostics.update(state.diagnostics)
                        for row in state.rows():
                            if start is not None and row["hour_utc"] < start or end is not None and row["hour_utc"] >= end:
                                continue
                            target = combined[key, row["hour_utc"], row["band"], row["mask_bit"]]
                            target[0] += row["vtec_tecu"] * row["valid_seconds"]
                            target[1] += row["valid_seconds"]
        rows = []
        for (key, hour, band, bit), (integral, seconds) in sorted(combined.items()):
            if not 0 < seconds <= 3600:
                raise ValueError("Continuous groups overlap within an hourly IGP average")
            lat, lon = state_coordinates(band, bit)
            rows.append(
                dict(
                    gnssId=key[0],
                    svId=key[1],
                    sigId=key[2],
                    freqId=key[3],
                    hour_utc=hour,
                    band=band,
                    mask_bit=bit,
                    latitude=lat,
                    longitude=lon,
                    vtec_tecu=integral / seconds,
                    valid_seconds=seconds,
                    coverage=seconds / 3600,
                )
            )
        return rows, diagnostics, messages
    finally:
        mapper.close()


def state_coordinates(band, bit):
    from .sbas_grid import COORDINATES

    return COORDINATES[band, bit]


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


def render(rows, coastline, output, vmin, vmax, min_coverage):
    selected = [row for row in rows if row["coverage"] >= min_coverage]
    if not selected:
        raise ValueError("No hourly grid cells meet the coverage threshold")
    extent = extent_for(selected)
    coast = list(coastline_parts(coastline))
    grouped = defaultdict(list)
    for row in selected:
        grouped[(row["gnssId"], row["svId"], row["sigId"], row["freqId"], row["hour_utc"])].append(row)
    image_dir = output / "png"
    image_dir.mkdir()
    manifest = []
    norm = Normalize(vmin=vmin, vmax=vmax, clip=True)
    cmap = matplotlib.colormaps["turbo"]
    for key, cells in tqdm(sorted(grouped.items()), desc="Render hourly PNG", unit="image"):
        gnss, sv, signal, frequency, hour = key
        directory = image_dir / f"gnss-{gnss}_sv-{sv}_sig-{signal}_freq-{frequency}"
        directory.mkdir(exist_ok=True)
        label = datetime.fromtimestamp(hour, timezone.utc).strftime("%Y-%m-%dT%H:00:00Z")
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
            title=f"SBAS PRN {sv} hourly mean VTEC — {label}\nvalid coverage ≥ {min_coverage:.0%}",
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
                "Title": f"SBAS PRN {sv} hourly mean VTEC {label}",
                "Description": "Experimental time-weighted MT26 grid; Made with Natural Earth.",
            },
        )
        plt.close(fig)
        manifest.append(
            dict(
                path=str(path.relative_to(output)),
                sha256=sha256(path),
                hour_utc=hour,
                gnssId=gnss,
                svId=sv,
                sigId=signal,
                freqId=frequency,
                cells=len(cells),
            )
        )
    return manifest, extent


def parse_hour(_context, _parameter, value):
    if value is None:
        return None
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H").replace(tzinfo=timezone.utc)
    except ValueError as error:
        raise click.BadParameter("use YYYY-MM-DDTHH in UTC") from error
    return int(parsed.timestamp())


@click.command()
@click.option("--sbas-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--reconstruction-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--coastline", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="New output directory.")
@click.option("--start", callback=parse_hour, help="Optional first UTC hour, YYYY-MM-DDTHH.")
@click.option("--end", callback=parse_hour, help="Optional exclusive UTC hour, YYYY-MM-DDTHH.")
@click.option("--vmin", type=float, default=0.0, show_default=True)
@click.option("--vmax", type=float, default=100.0, show_default=True)
@click.option("--min-coverage", type=click.FloatRange(0, 1), default=0.25, show_default=True)
def cli(sbas_dir, reconstruction_dir, coastline, output, start, end, vmin, vmax, min_coverage):
    """Export time-weighted hourly SBAS VTEC maps as PNG files."""
    try:
        if start is not None and end is not None and start >= end:
            raise ValueError("--end must be later than --start")
        if vmax <= vmin:
            raise ValueError("--vmax must exceed --vmin")
        output.mkdir(parents=False, exist_ok=False)
        rows, diagnostics, messages = aggregate(sbas_dir.resolve(), reconstruction_dir.resolve(), start, end)
        with (output / "hourly.jsonl").open("x") as stream:
            for row in rows:
                stream.write(json.dumps(row, sort_keys=True) + "\n")
        manifest, extent = render(rows, coastline.resolve(), output, vmin, vmax, min_coverage)
        source_files = [Path(__file__).resolve(), Path(__file__).with_name("sbas_grid.py").resolve()]
        provenance = output / "provenance"
        provenance.mkdir()
        for source in source_files:
            shutil.copy2(source, provenance / source.name)
        run = dict(
            schema=1,
            status="complete",
            arguments=sys.argv,
            git_revision=subprocess.check_output(
                ["git", "-C", str(Path(__file__).resolve().parents[2]), "rev-parse", "HEAD"], text=True
            ).strip(),
            source_sha256={path.name: sha256(path) for path in source_files},
            sbas_completed_sha256=sha256(sbas_dir / "completed.json"),
            reconstruction_completed_sha256=sha256(reconstruction_dir / "completed.json"),
            coastline=str(coastline.resolve()),
            coastline_sha256=sha256(coastline),
            dependencies={name: importlib.metadata.version(name) for name in ("matplotlib", "numpy", "pyshp")},
            policy=dict(
                correction_age_seconds=600,
                mask_age_seconds=1200,
                mean="valid-time-weighted sample-and-hold",
                min_coverage=min_coverage,
                vmin=vmin,
                vmax=vmax,
            ),
            extent=extent,
            hourly_cells=len(rows),
            images=len(manifest),
            message_types=dict(messages),
            diagnostics=dict(diagnostics),
        )
        write_json(output / "images.json", {"images": manifest})
        write_json(output / "completed.json", run)
        click.echo(
            json.dumps(
                {
                    "status": "complete",
                    "hourly_cells": len(rows),
                    "images": len(manifest),
                    "message_types": dict(messages),
                    "diagnostics": dict(diagnostics),
                }
            )
        )
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
