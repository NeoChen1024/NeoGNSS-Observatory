#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Render per-source SBAS snapshot products with presentation-time selection."""

import json
import math
import os
from pathlib import Path

import click

from .map_assets import with_coastline
from .research_output import staged_output, write_json
from .sbas_composite import (
    PRIORITY,
    grid_files,
    iter_snapshots,
    parse_priority,
    select_snapshot,
)
from .sbas_grid_render import extent_for, parse_hour


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option(
    "--coastline", type=click.Path(exists=True, dir_okay=False, path_type=Path), help="Override bundled Natural Earth coastline."
)
@click.option("--start", callback=parse_hour, help="First GPST hour, YYYY-MM-DDTHH.")
@click.option("--end", callback=parse_hour, help="Exclusive final GPST hour, YYYY-MM-DDTHH.")
@click.option("--priority", default=",".join(PRIORITY), callback=parse_priority, show_default=True)
@click.option("--quantity", type=click.Choice(["current", "mean"]), default="current", show_default=True)
@click.option("--vmin", type=float, default=0, show_default=True)
@click.option("--vmax", type=float, default=200, show_default=True)
@click.option("--min-coverage", type=click.FloatRange(0, 1), default=0.0, show_default=True)
@click.option("--workers", type=click.IntRange(1, 32), default=min(4, os.cpu_count() or 1), show_default=True)
@click.option("--png-compression", type=click.IntRange(0, 9), default=3, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace completed output while retaining its backup.")
@with_coastline
@staged_output
def cli(input_dir, output, coastline, start, end, priority, quantity, vmin, vmax, min_coverage, workers, png_compression):
    """Render SBAS snapshot maps; mean selects already-aggregated source means."""
    try:
        if start is not None and end is not None and start >= end:
            raise ValueError("--end must exceed --start")
        if not math.isfinite(vmin) or not math.isfinite(vmax) or vmax <= vmin:
            raise ValueError("Require finite --vmin < --vmax")
        files = grid_files(input_dir)
        if not files:
            raise ValueError("No SBAS grid snapshot Parquet found")

        def selected():
            for path in files:
                for stamp, rows in iter_snapshots(path):
                    if (start is None or stamp >= start) and (end is None or stamp < end):
                        cells = select_snapshot(rows, priority, quantity, min_coverage)
                        if cells:
                            yield cells

        # First pass finds a common extent. Never retain an archive in RAM.
        extent = None
        for cells in selected():
            bound = extent_for(cells)
            if extent is None:
                extent = bound
            else:
                extent = min(extent[0], bound[0]), max(extent[1], bound[1]), min(extent[2], bound[2]), max(extent[3], bound[3])
        if extent is None:
            raise ValueError("No snapshot cells match the selection")
        output.mkdir(parents=True, exist_ok=False)
        images = []
        import multiprocessing
        from concurrent.futures import ProcessPoolExecutor

        from .sbas_grid_render import initialize_coastline, render_job

        with ProcessPoolExecutor(
            max_workers=workers,
            mp_context=multiprocessing.get_context("spawn"),
            initializer=initialize_coastline,
            initargs=(coastline, extent),
        ) as pool:
            # Bounded work submission also bounds retained snapshot dictionaries.
            pending = []
            for cells in selected():
                pending.append(
                    pool.submit(render_job, (cells, coastline, output, vmin, vmax, min_coverage, extent, png_compression))
                )
                if len(pending) >= workers * 2:
                    images.extend(pending.pop(0).result())
            for future in pending:
                images.extend(future.result())
        write_json(output / "images.json", dict(time_scale="GPST", quantity=quantity, images=images))
        click.echo(json.dumps(dict(status="complete", images=len(images), quantity=quantity)))
    except (OSError, ValueError, KeyError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
