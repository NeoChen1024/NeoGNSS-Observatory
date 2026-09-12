# SPDX-License-Identifier: GPL-3.0-only
"""Render hourly SBAS VTEC maps using only daily grid interval Parquet."""

import json
import math
import os
import zipfile
from collections import defaultdict
from pathlib import Path

import click
import pyarrow.parquet as pq
from tqdm import tqdm

from .research_output import staged_output, write_json
from .sbas_grid_parquet import SCHEMA
from .sbas_grid_render import extent_for, parse_hour, render_job
from .sbas_streams import IDENTITY


def hourly_rows(path, day, start=None, end=None):
    """Aggregate one bounded GPST day; zero coverage remains absent, not zero TEC."""
    if day % 86400000:
        raise ValueError("Parquet partition must start at GPST midnight")
    parquet = pq.ParquetFile(path)
    if not parquet.schema_arrow.equals(SCHEMA) or any(
        parquet.schema_arrow.metadata.get(k) != v for k, v in SCHEMA.metadata.items()
    ):
        raise ValueError(f"Unexpected SBAS Parquet schema: {path}")
    stats = defaultdict(lambda: [0.0, 0, None])
    last_end = {}
    for batch in parquet.iter_batches(batch_size=65536):
        for row in batch.to_pylist():
            begin, finish = row["start_gpst_ms"], row["end_gpst_ms"]
            if not day <= begin < finish <= day + 86400000:
                raise ValueError("SBAS interval outside its GPST day")
            key = tuple(row[k] for k in (*IDENTITY, "band", "mask_bit"))
            if begin < last_end.get(key, begin):
                raise ValueError("Overlapping or reversed SBAS grid intervals")
            last_end[key] = finish
            if not math.isfinite(row["vtec_tecu"]) or row["vtec_tecu"] < 0:
                raise ValueError("Invalid SBAS VTEC")
            while begin < finish:
                hour = begin // 3600000 * 3600000
                stop = min(finish, hour + 3600000)
                if (start is None or hour >= start * 1000) and (end is None or hour < end * 1000):
                    stat = stats[key, hour]
                    stat[0] += row["vtec_tecu"] * (stop - begin) / 1000
                    stat[1] += stop - begin
                    stat[2] = row
                begin = stop
    result = []
    for (key, hour), (integral, milliseconds, row) in sorted(stats.items()):
        if not 0 < milliseconds <= 3600000:
            raise ValueError("Invalid SBAS hourly coverage")
        result.append(
            dict(
                zip((*IDENTITY, "band", "mask_bit"), key),
                hour_gpst=hour // 1000,
                latitude=row["latitude"],
                longitude=row["longitude"],
                vtec_integral_tecu_seconds=integral,
                valid_seconds=milliseconds / 1000,
                coverage=milliseconds / 3600000,
                vtec_tecu=integral / (milliseconds / 1000),
            )
        )
    return result


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--coastline", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--start", callback=parse_hour, help="First GPST hour, YYYY-MM-DDTHH.")
@click.option("--end", callback=parse_hour, help="Exclusive final GPST hour, YYYY-MM-DDTHH.")
@click.option("--vmin", type=float, default=0, show_default=True)
@click.option("--vmax", type=float, default=200, show_default=True)
@click.option("--min-coverage", type=click.FloatRange(0, 1), default=0.25, show_default=True)
@click.option("--workers", type=click.IntRange(1, 32), default=min(4, os.cpu_count() or 1), show_default=True)
@click.option("--png-compression", type=click.IntRange(0, 9), default=3, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, coastline, start, end, vmin, vmax, min_coverage, workers, png_compression):
    """Read daily GPST Parquet and export time-weighted hourly PNG maps."""
    import multiprocessing
    from concurrent.futures import ProcessPoolExecutor

    try:
        if start is not None and end is not None and start >= end:
            raise ValueError("--end must exceed --start")
        if not math.isfinite(vmin) or not math.isfinite(vmax) or vmax <= vmin:
            raise ValueError("Require finite --vmin < --vmax")
        daily = input_dir / "daily" if (input_dir / "daily").is_dir() else input_dir
        files = []
        for path in sorted(daily.glob("GPST-*.parquet")):
            metadata = pq.ParquetFile(path).schema_arrow.metadata or {}
            day = int(metadata[b"day_gpst_ms"])
            if (start is None or day + 86400000 > start * 1000) and (end is None or day < end * 1000):
                files.append(dict(path=str(path.resolve()), day_gpst_ms=day))
        output.mkdir(parents=False, exist_ok=False)
        cache = output / "hourly"
        cache.mkdir()
        daily_cache, bounds = [], []
        for record in tqdm(sorted(files, key=lambda r: r["day_gpst_ms"]), desc="Aggregate GPST days", unit="day"):
            path = (input_dir / record["path"]).resolve()
            rows = hourly_rows(path, record["day_gpst_ms"], start, end)
            target = cache / (path.stem + ".jsonl")
            with target.open("x") as stream:
                for row in rows:
                    stream.write(json.dumps(row) + "\n")
            daily_cache.append(target)
            visible = [r for r in rows if r["coverage"] >= min_coverage]
            if visible:
                bounds.append(extent_for(visible))
        if not bounds:
            raise ValueError("No hourly cells meet the coverage threshold")
        extent = (min(b[0] for b in bounds), max(b[1] for b in bounds), min(b[2] for b in bounds), max(b[3] for b in bounds))
        images = []
        with ProcessPoolExecutor(max_workers=workers, mp_context=multiprocessing.get_context("spawn")) as pool:
            for path in tqdm(daily_cache, desc="Render GPST days", unit="day"):
                grouped = defaultdict(list)
                with path.open() as stream:
                    for line in stream:
                        row = json.loads(line)
                        if row["coverage"] >= min_coverage:
                            grouped[tuple(row[k] for k in (*IDENTITY, "hour_gpst"))].append(row)
                jobs = [
                    (cells, coastline, output, vmin, vmax, min_coverage, extent, png_compression)
                    for _, cells in sorted(grouped.items())
                ]
                for manifest in pool.map(render_job, jobs):
                    images.extend(manifest)
        write_json(output / "images.json", dict(time_scale="GPST", images=images))
        write_json(
            output / "completed.json",
            dict(
                schema=1,
                status="complete",
                time_scale="GPST",
                input_files=files,
                images=len(images),
                extent=extent,
                hourly_files=[dict(path=str(p.relative_to(output))) for p in daily_cache],
                policy=dict(
                    start=start,
                    end=end,
                    vmin=vmin,
                    vmax=vmax,
                    min_coverage=min_coverage,
                    workers=workers,
                    png_compression=png_compression,
                    mean="valid-time-weighted sample-and-hold",
                ),
            ),
        )
        click.echo(json.dumps(dict(status="complete", days=len(files), images=len(images))))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
