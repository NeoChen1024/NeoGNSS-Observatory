# SPDX-License-Identifier: GPL-3.0-only
"""Render hourly SBAS VTEC maps using only daily grid interval Parquet."""

import json
import math
import os
import zipfile
from collections import defaultdict
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from .map_assets import with_coastline
from .research_output import staged_output, write_json
from .sbas_composite import (
    HOURLY_SCHEMA,
    PRIORITY,
    SELECTED_SCHEMA,
    composite_day,
    parse_priority,
)
from .sbas_grid_render import extent_for, initialize_coastline, parse_hour, render_job


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option(
    "--coastline",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    help="Override the bundled Natural Earth 10m coastline ZIP.",
)
@click.option("--start", callback=parse_hour, help="First GPST hour, YYYY-MM-DDTHH.")
@click.option("--end", callback=parse_hour, help="Exclusive final GPST hour, YYYY-MM-DDTHH.")
@click.option(
    "--priority", default=",".join(PRIORITY), callback=parse_priority, show_default=True, help="Provider preference, highest first."
)
@click.option("--vmin", type=float, default=0, show_default=True)
@click.option("--vmax", type=float, default=200, show_default=True)
@click.option("--min-coverage", type=click.FloatRange(0, 1), default=0.25, show_default=True)
@click.option("--workers", type=click.IntRange(1, 32), default=min(4, os.cpu_count() or 1), show_default=True)
@click.option("--png-compression", type=click.IntRange(0, 9), default=3, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@with_coastline
@staged_output
def cli(input_dir, output, coastline, start, end, priority, vmin, vmax, min_coverage, workers, png_compression):
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
        available = {}
        for path in sorted(daily.glob("GPST-*.parquet")):
            metadata = pq.ParquetFile(path).schema_arrow.metadata or {}
            day = int(metadata[b"day_gpst_ms"])
            if day in available:
                raise ValueError("Duplicate SBAS day")
            available[day] = path
            if (start is None or day + 86400000 > start * 1000) and (end is None or day < end * 1000):
                files.append(dict(path=str(path.resolve()), day_gpst_ms=day))
        output.mkdir(parents=False, exist_ok=False)
        cache = output / "hourly"
        cache.mkdir()
        selections = output / "selected"
        selections.mkdir()
        daily_cache, bounds, state = [], [], {}
        if files:
            previous = min(r["day_gpst_ms"] for r in files) - 86400000
            if previous in available:
                composite_day(available[previous], previous, priority=priority, state=state)
        hourly_schema = HOURLY_SCHEMA.with_metadata({**HOURLY_SCHEMA.metadata, b"priority": ",".join(priority).encode()})
        selected_schema = SELECTED_SCHEMA.with_metadata({**SELECTED_SCHEMA.metadata, b"priority": ",".join(priority).encode()})
        for record in tqdm(sorted(files, key=lambda r: r["day_gpst_ms"]), desc="Aggregate GPST days", unit="day"):
            path = (input_dir / record["path"]).resolve()
            rows, selected = composite_day(path, record["day_gpst_ms"], start, end, priority, state)
            pq.write_table(
                pa.Table.from_pylist(selected, schema=selected_schema),
                selections / path.name,
                compression="zstd",
                compression_level=3,
                use_dictionary=True,
            )
            target = cache / (path.stem + ".parquet")
            pq.write_table(
                pa.Table.from_pylist(rows, schema=hourly_schema),
                target,
                compression="zstd",
                compression_level=3,
                use_dictionary=True,
            )
            daily_cache.append(target)
            visible = [r for r in rows if r["coverage"] >= min_coverage]
            if visible:
                bounds.append(extent_for(visible))
        if not bounds:
            raise ValueError("No hourly cells meet the coverage threshold")
        extent = (min(b[0] for b in bounds), max(b[1] for b in bounds), min(b[2] for b in bounds), max(b[3] for b in bounds))
        images = []
        with ProcessPoolExecutor(
            max_workers=workers,
            mp_context=multiprocessing.get_context("spawn"),
            initializer=initialize_coastline,
            initargs=(coastline, extent),
        ) as pool:
            for path in tqdm(daily_cache, desc="Render GPST days", unit="day"):
                grouped = defaultdict(list)
                with pq.ParquetFile(path) as parquet:
                    for batch in parquet.iter_batches(batch_size=65536):
                        for row in batch.to_pylist():
                            if row["coverage"] >= min_coverage:
                                grouped[row["hour_gpst"]].append(row)
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
                schema=2,
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
                    mean="source selection before valid-time-weighted hourly average",
                    priority=list(priority),
                    mt0_policy="research; retained and flagged, no integrity assurance",
                ),
            ),
        )
        click.echo(json.dumps(dict(status="complete", days=len(files), images=len(images))))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
