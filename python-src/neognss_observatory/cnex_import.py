#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Experimental CommonNEX import; no receiver acquisition or deduplication."""

import json
import re
import shutil
import sys
import tempfile
from collections import OrderedDict
from datetime import date, timedelta
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .setup_antex import select_antenna
from .setup_metadata import component, validate_setup
from .setup_tracking import populate_tracking

PATTERN = re.compile(r"r([0-9]{2})-([a-z][a-z0-9-]*)-part([0-9]{2})\.parquet\Z")
ORIGIN = date(1980, 1, 6)


def gpst_text(parts):
    seconds, fraction = parts
    return f"{seconds}.{fraction:012d} GPST seconds"


def ordered_inputs(paths, protocol):
    """Stable sort by a bounded head probe, without sharing decoder state."""
    found = []
    for path in tqdm(paths, desc="Probe input heads", unit="file", file=sys.stderr):
        size = path.stat().st_size
        limit = min(size, max((size + 99) // 100, 1024**2))
        probe = _native.CnexTimeProbe(protocol)
        result = {"observation": None, "navigation": None}
        with path.open("rb") as source:
            remaining = limit
            while remaining:
                data = source.read(min(1024**2, remaining))
                if not data:
                    raise ValueError(f"Input shortened while probing: {path}")
                remaining -= len(data)
                probe.feed(data)
                result = probe.result()
                if result["observation"] is not None:
                    break
        if path.stat().st_size != size:
            raise ValueError(f"Input changed while probing: {path}")
        kind = "observation" if result["observation"] is not None else "navigation"
        time = result[kind]
        if time is None:
            raise ValueError(
                f"No usable GPST in the first {limit} bytes of {path}; inspect/re-stitch the input. No filename fallback."
            )
        found.append((tuple(time), path, size, kind))
    found.sort(key=lambda item: item[0])
    click.echo("Input order (head timestamps; no overlap removal):", err=True)
    for time, path, _, kind in found:
        click.echo(f"  {gpst_text(time)} [{kind}] {path}", err=True)
    return [(path, 0, size) for _, path, size, _ in found]


def reversal_message(error, segments):
    offset = error["offset"]
    location = f"concatenated input byte {offset}"
    for path, start, size in segments:
        if offset < size - start:
            location = f"{path} at byte {start + offset}"
            break
        offset -= size - start
    return (
        f"{error['axis'].capitalize()} GPST moved backwards at {location}: "
        f"{gpst_text(error['previous'])} -> {gpst_text(error['current'])}. "
        "Possible overlap or timestamp disorder; inspect/re-stitch the inputs before importing."
    )


def parse_name(name):
    match = PATTERN.fullmatch(name)
    if not match:
        raise ValueError(f"Invalid ParquetNEX filename: {name}")
    revision, catalog, part = match.groups()
    return int(revision), catalog, int(part)


def latest_parts(directory, catalog):
    found = []
    for path in directory.glob("*.parquet"):
        revision, family, part = parse_name(path.name)
        if family == catalog:
            found.append((revision, part, path))
    if not found:
        return []
    revision = max(item[0] for item in found)
    return [path for rev, part, path in sorted(found) if rev == revision]


def next_name(directory, catalog, mode):
    parts = latest_parts(directory, catalog)
    if not parts:
        revision, part = 0, 0
    else:
        revision, _, last = parse_name(parts[-1].name)
        if mode == "new":
            raise ValueError(f"Catalog already exists: {directory}/{catalog}; use explicit rebuild or tail continuation")
        revision, part = (revision + 1, 0) if mode == "rebuild" else (revision, last + 1)
    if revision > 99 or part > 99:
        raise ValueError("Two-digit revision/part space exhausted")
    return f"r{revision:02d}-{catalog}-part{part:02d}.parquet"


def write_json(path, value):
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("x", encoding="utf-8") as out:
        json.dump(value, out, ensure_ascii=False, indent=2)
        out.write("\n")
    temporary.replace(path)


@click.group()
def cli():
    """Import local UBX/SBF observations and canonical RawBits into ParquetNEX."""


@cli.command("init")
@click.argument("output", type=click.Path(path_type=Path))
@click.option("--setup", "setup_file", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option(
    "--vendor-config",
    type=click.Path(exists=True, dir_okay=False, readable=True, path_type=Path),
    help="Copy this file beside setup.json and override vendor_config with its filename.",
)
@click.option(
    "--antenna-catalog",
    "antenna_catalogs",
    multiple=True,
    type=click.Path(exists=True, dir_okay=False, readable=True, path_type=Path),
    help="ANTEX 1.4 absolute catalog (.atx or .atx.gz); repeat in preferred order. A single-antenna file is also accepted.",
)
def initialize(output, setup_file, vendor_config, antenna_catalogs):
    """Initialize one logical station with one receiver and antenna."""
    stage = None
    try:
        if output.exists() or output.is_symlink():
            raise ValueError(f"Output already exists: {output}")
        setup = json.loads(setup_file.read_text(encoding="utf-8"))
        if not isinstance(setup, dict):
            raise ValueError("Setup must be a JSON object")
        if vendor_config is not None:
            setup["vendor_config"] = vendor_config.name
        validate_setup(setup)
        config = setup.get("vendor_config")
        config_source = None
        if config:
            component(config)
            config_source = vendor_config if vendor_config is not None else setup_file.parent / config
            if (
                config in ("setup.json", ".setup.json.tmp", "antenna.atx")
                or re.fullmatch(r"\d{4}-\d{2}-\d{2}", config)
                or not config_source.is_file()
            ):
                raise ValueError("Invalid vendor_config filename")
        populate_tracking(setup, config_source, lambda message: click.echo(f"Warning: {message}", err=True))
        antenna = setup["antenna"]
        if not antenna_catalogs and antenna.get("calibration_file"):
            antenna_catalogs = (setup_file.parent / component(antenna["calibration_file"]),)
        calibration = None
        if antenna_catalogs:
            calibration, catalog, count, kind = select_antenna(antenna_catalogs, antenna)
            antenna["calibration_file"] = "antenna.atx"
            click.echo(f"Selected {count} {kind} antenna calibration record(s) from {catalog}", err=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=".cnex-init-", dir=output.parent))
        write_json(stage / "setup.json", setup)
        if config:
            shutil.copyfile(config_source, stage / config)
        if calibration is not None:
            with (stage / "antenna.atx").open("xb") as target:
                target.write(calibration)
        stage.rename(output)
        stage = None
        click.echo(str(output))
    except (ValueError, KeyError, OSError) as exc:
        raise click.ClickException(str(exc)) from exc
    finally:
        if stage is not None:
            shutil.rmtree(stage)  # Only this initializer's unpublished temporary directory.


@cli.command("list")
@click.argument("station", type=click.Path(exists=True, file_okay=False, path_type=Path))
def list_parts(station):
    """List reader-selected latest revisions and all their parts."""
    try:
        for day in sorted(station.iterdir()):
            if not day.is_dir() or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", day.name):
                continue
            date.fromisoformat(day.name)
            catalogs = {parse_name(p.name)[1] for p in day.glob("*.parquet")}
            for catalog in sorted(catalogs):
                for path in latest_parts(day, catalog):
                    click.echo(str(path))
    except (ValueError, OSError) as exc:
        raise click.ClickException(str(exc)) from exc


@cli.command("run")
@click.argument("inputs", nargs=-1, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--station", required=True, type=click.Path(exists=True, file_okay=False, path_type=Path))
@click.option("--protocol", "-p", type=click.Choice(["ubx", "sbf"]), default="ubx", show_default=True)
@click.option("--rebuild", is_flag=True, help="Replace affected daily catalogs with a new complete revision.")
@click.option(
    "--resume-from",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    help="Continue using a daily import-state.json, replaying its raw tail first.",
)
@click.option("--chunk-mib", type=click.IntRange(1, 64), default=4, show_default=True)
@click.option(
    "--source-antenna",
    type=click.IntRange(0, 7),
    default=0,
    show_default=True,
    help="Native observation input to import into this station; RAWX requires 0.",
)
def run(inputs, station, protocol, rebuild, resume_from, chunk_mib, source_antenna):
    """Head-probe and time-sort INPUTS, then import one continuous recording path.

    Catalogs: observations (including MeasExtra), raw-bits and
    observation/navigation completion events. See the RawBits coverage table
    for documented and sample-verified adapters. Telemetry and cadence/clock
    events are not yet imported.
    No automatic overlap merging. Rebuild requires the complete replacement input.
    """
    writers = {}
    closed = []
    active = OrderedDict()
    schemas = {}
    tail_limits = {}
    stage = None
    try:
        station = station.resolve()
        setup = validate_setup(json.loads((station / "setup.json").read_text(encoding="utf-8")))
        if rebuild and resume_from:
            raise ValueError("Rebuild and continuation are mutually exclusive")
        if not inputs:
            raise ValueError("Supply local raw input files")
        new_paths = [p.resolve() for p in inputs]
        if len(set(new_paths)) != len(new_paths):
            raise ValueError("Repeated input path; no automatic deduplication")
        if any(p.suffix == ".xz" for p in new_paths):
            raise ValueError("Provide expanded raw files, not XZ archives")
        state = None
        segments = []
        if resume_from:
            state = json.loads(resume_from.read_text(encoding="utf-8"))
            if (
                state["protocol"] != protocol
                or state["station"] != str(station)
                or state["source_antenna"] != source_antenna
                or state["setup_id"] != setup["setup_id"]
                or state.get("continued")
            ):
                raise ValueError("Incompatible or already consumed continuation state")
            for name in state["published_files"]:
                path = station / name
                if path not in latest_parts(path.parent, parse_name(path.name)[1]):
                    raise ValueError("Continuation state refers to an obsolete revision")
            for item in state["tail_inputs"]:
                path = Path(item["path"])
                if path in new_paths or path.stat().st_size != item["size"]:
                    raise ValueError("Tail input changed or was also supplied as new input")
                segments.append((path, item["offset"], item["size"]))
        # Saved raw tail is replay context, not a candidate for fresh sorting.
        segments.extend(ordered_inputs(new_paths, protocol))
        mode = "rebuild" if rebuild else "tail" if resume_from else "new"
        stage = Path(tempfile.mkdtemp(prefix=".cnex-import-", dir=station))
        reader = _native.CnexObservationReader(protocol, setup["setup_id"], source_antenna)
        if state:
            reader.restore(state["navigation_context"])
        counts = {"observations": 0, "events": 0, "raw-bits": 0}

        def time_column(catalog):
            return "nav_epoch_gpst" if catalog == "raw-bits" else "gpst"

        def get_writer(key, schema):
            if key not in writers or writers[key][0] is None:
                if len(active) >= 8:
                    victim, _ = active.popitem(last=False)
                    writer, temporary, target = writers[victim]
                    writer.close()
                    closed.append((None, temporary, target))
                    writers[victim] = (None, temporary, target)
                day_name, catalog = key
                directory = station / day_name
                if key in writers:
                    revision, _, part = parse_name(writers[key][2].name)
                    if part == 99:
                        raise ValueError("Two-digit part space exhausted")
                    name = f"r{revision:02d}-{catalog}-part{part + 1:02d}.parquet"
                else:
                    name = next_name(directory, catalog, mode)
                target = directory / name
                temporary = stage / f"{day_name}-{name}"
                schema = schema.with_metadata(
                    {
                        "commonnex.schema": "observation-pilot-1",
                        "commonnex.catalog": catalog,
                        "time.scale": "GPST",
                        "time.origin": "1980-01-06T00:00:00",
                        "time.unit": "s",
                        "setup_id": setup["setup_id"],
                    }
                )
                writers[key] = (pq.ParquetWriter(temporary, schema, compression="zstd", compression_level=3), temporary, target)
                active[key] = None
            active.move_to_end(key)
            return writers[key][0]

        def check_tail(key, batch):
            if mode != "tail":
                return
            if key not in tail_limits:
                maxima = []
                for path in latest_parts(station / key[0], key[1]):
                    with pq.ParquetFile(path) as old:
                        index = old.schema.names.index(time_column(key[1]))
                        for i in range(old.metadata.num_row_groups):
                            stat = old.metadata.row_group(i).column(index).statistics
                            if stat and stat.has_min_max:
                                maxima.append(stat.max)
                            elif old.metadata.row_group(i).num_rows:
                                raise ValueError("Missing GPST statistics for tail validation")
                tail_limits[key] = max(maxima) if maxima else None
            limit = tail_limits[key]
            if limit is not None and pc.min(batch.column(time_column(key[1]))).as_py() < limit:
                raise ValueError("Continuation would insert before published tail; use a complete rebuild")

        warned_foreign = False
        total = sum(size - start for _, start, size in segments)
        click.echo(
            "Importing observations, canonical RawBits and independent completion events.",
            err=True,
        )
        with tqdm(total=total, unit="B", unit_scale=True, desc="CommonNEX", file=sys.stderr) as progress:
            for path, start, size in segments:
                with path.open("rb") as source:
                    source.seek(start)
                    remaining = size - start
                    while remaining:
                        data = source.read(min(chunk_mib * 1024**2, remaining))
                        if not data:
                            raise ValueError(f"Input shortened while reading: {path}")
                        remaining -= len(data)
                        try:
                            native_batches = reader.feed(data)
                        except RuntimeError as error:
                            reversal = reader.time_error()
                            if reversal:
                                raise ValueError(reversal_message(reversal, segments)) from error
                            raise RuntimeError(f"{path}: {error}") from error
                        for catalog, native_batch in zip(counts, native_batches):
                            batch = pa.record_batch(native_batch)
                            schemas[catalog] = batch.schema
                            if not batch.num_rows:
                                continue
                            seconds = pc.cast(batch.column(time_column(catalog)), pa.int64(), safe=False).to_numpy()
                            days = seconds // 86400
                            for day in np.unique(days):
                                day_name = (ORIGIN + timedelta(days=int(day))).isoformat()
                                key = day_name, catalog
                                selected = batch.filter(pa.array(days == day))
                                check_tail(key, selected)
                                get_writer(key, batch.schema).write_batch(selected)
                            counts[catalog] += batch.num_rows
                        summary = reader.summary()
                        if summary["skipped_protocol_frames"] and not warned_foreign:
                            click.echo("Warning: skipped complete foreign-protocol frames", err=True)
                            warned_foreign = True
                        progress.update(len(data))
                if path.stat().st_size != size:
                    raise ValueError(f"Input changed while reading: {path}")
        summary = reader.summary()
        if not writers:
            raise ValueError(f"No completed supported records: {summary}")
        if rebuild:
            for day_name in {key[0] for key in writers}:
                for catalog in counts:
                    if (day_name, catalog) not in writers and latest_parts(station / day_name, catalog):
                        get_writer((day_name, catalog), schemas[catalog])
        for writer, _, _ in writers.values():
            if writer is not None:
                writer.close()
        tail = []
        offset = summary["resume_offset"]
        for path, start, size in segments:
            length = size - start
            if offset < length:
                tail.append({"path": str(path), "offset": start + offset, "size": size})
                offset = 0
            else:
                offset -= length
        published = []
        for _, temporary, target in closed + [v for v in writers.values() if v[0] is not None]:
            target.parent.mkdir(exist_ok=True)
            if target.exists():
                raise ValueError(f"Refusing to overwrite: {target}")
            temporary.rename(target)
            published.append(str(target.relative_to(station)))
        continuation = {
            "version": 1,
            "station": str(station),
            "protocol": protocol,
            "setup_id": setup["setup_id"],
            "source_antenna": source_antenna,
            "published_files": published,
            "tail_inputs": tail,
            "input_end": {"path": str(segments[-1][0]), "offset": segments[-1][2]},
            "continued": False,
            "navigation_context": reader.checkpoint(),
        }
        # The latest affected day owns this invocation's continuation cursor.
        last_day = max(key[0] for key in writers)
        next_state = station / last_day / "import-state.json"
        if resume_from and resume_from.resolve() != next_state:
            write_json(resume_from, {**state, "continued": True})
        write_json(next_state, continuation)
        stage.rmdir()
        click.echo(json.dumps({"status": "complete", **counts, **summary, "state": str(next_state)}))
        if summary["pending_epoch"] or summary["pending_frame_bytes"] or summary["incomplete_epochs"]:
            click.echo("Warning: incomplete frames/groups withheld; inspect summary and continuation state", err=True)
        if summary["unsupported_signals"] or summary["untimed_epochs"] or summary["invalid_frames"]:
            click.echo("Warning: unsupported/untimed/invalid input was encountered; inspect summary", err=True)
        if summary["measextra_unmatched"] or summary["measextra_ambiguous"] or summary["measextra_unsupported"]:
            click.echo("Warning: some MeasExtra records could not be associated; inspect summary", err=True)
        if summary["raw_bits_untimed"] or summary["raw_bits_unsupported"] or summary["raw_bits_pending"]:
            click.echo("Warning: RawBits records were withheld or unsupported; inspect counts and navigation context", err=True)
    except (ValueError, KeyError, OSError, RuntimeError, pa.ArrowException) as exc:
        for writer, _, _ in writers.values():
            if writer is not None:
                writer.close()
        if stage:
            click.echo(f"Incomplete staging retained at {stage}; resolve interrupted publication before reading", err=True)
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
