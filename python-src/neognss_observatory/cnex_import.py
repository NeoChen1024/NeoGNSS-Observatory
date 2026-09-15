#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Experimental CommonNEX import; no receiver acquisition or deduplication."""

import json
import os
import re
import shutil
import sys
import tempfile
from collections import OrderedDict, deque
from concurrent.futures import ThreadPoolExecutor
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
SBF_FILENAME = re.compile(r"[A-Za-z0-9_]{4}[0-9]{3}[A-Za-z0-9]\.[0-9]{2}_\Z")


def discover_inputs(inputs, protocol, recursive):
    """Discover protocol-specific filenames; payload probes determine ordering."""
    paths = []

    def walk_error(error):
        raise error

    for path in inputs:
        if path.is_dir():
            if not recursive:
                raise ValueError(f"Directory input requires --recursive/-r: {path}")
            for directory, directories, files in os.walk(path, followlinks=False, onerror=walk_error):
                directories.sort()
                for name in sorted(files):
                    matches = name.endswith(".ubx") if protocol == "ubx" else SBF_FILENAME.fullmatch(name)
                    candidate = Path(directory) / name
                    if matches and candidate.is_file():
                        paths.append(candidate.resolve())
        elif path.is_file():
            paths.append(path.resolve())
        else:
            raise ValueError(f"Input is not a regular file or directory: {path}")
    if not paths:
        raise ValueError(f"No matching expanded {protocol.upper()} input files")
    if len(set(paths)) != len(paths):
        raise ValueError("Repeated input path; do not supply overlapping directories or the same file twice")
    if any(p.suffix == ".xz" for p in paths):
        raise ValueError("Provide expanded raw files, not XZ archives")
    return paths


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
        f"{error['axis']} GPST moved backwards at {location}: "
        f"{gpst_text(error['previous'])} -> {gpst_text(error['current'])}. "
        "Possible overlap or timestamp disorder; inspect/re-stitch the inputs before importing."
    )


def parse_name(name):
    match = PATTERN.fullmatch(name)
    if not match:
        raise ValueError(f"Invalid ParquetNEX filename: {name}")
    revision, catalog, part = match.groups()
    return int(revision), catalog, int(part)


def day_directories(station):
    for path in sorted(station.glob("[0-9][0-9][0-9][0-9]/[0-9][0-9]/[0-9][0-9]")):
        if path.is_dir():
            date.fromisoformat("-".join(path.relative_to(station).parts))
            yield path


def day_path(day):
    return (ORIGIN + timedelta(days=int(day))).strftime("%Y/%m/%d")


def dictionary_columns(schema):
    """Keep dictionaries for text/bytes, never for numeric physical columns."""
    columns = []

    def visit(field, path):
        typ = field.type
        if pa.types.is_struct(typ):
            for child in typ:
                visit(child, f"{path}.{child.name}")
        elif pa.types.is_list(typ) or pa.types.is_large_list(typ) or pa.types.is_fixed_size_list(typ):
            visit(typ.value_field, f"{path}.list.element")
        elif (
            pa.types.is_string(typ)
            or pa.types.is_large_string(typ)
            or pa.types.is_binary(typ)
            or pa.types.is_large_binary(typ)
            or pa.types.is_fixed_size_binary(typ)
        ):
            columns.append(path)

    for field in schema:
        visit(field, field.name)
    return columns


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
                or re.fullmatch(r"\d{4}", config)
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
        for day in day_directories(station):
            catalogs = {parse_name(p.name)[1] for p in day.glob("*.parquet")}
            for catalog in sorted(catalogs):
                for path in latest_parts(day, catalog):
                    click.echo(str(path))
    except (ValueError, OSError) as exc:
        raise click.ClickException(str(exc)) from exc


@cli.command("run")
@click.argument("inputs", nargs=-1, type=click.Path(exists=True, path_type=Path))
@click.option(
    "--recursive",
    "-r",
    is_flag=True,
    help="Recursively discover *.ubx or SBF marker+DOY+session.YY_ files in directory inputs; ignore XZ copies.",
)
@click.option("--station", required=True, type=click.Path(exists=True, file_okay=False, path_type=Path))
@click.option("--protocol", "-p", type=click.Choice(["ubx", "sbf"]), default="ubx", show_default=True)
@click.option("--rebuild", is_flag=True, help="Replace affected daily catalogs with a new complete revision.")
@click.option(
    "--resume-from",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    help="Override automatic latest-state selection; replay the saved raw tail first.",
)
@click.option("--chunk-mib", type=click.IntRange(1, 64), default=4, show_default=True)
@click.option(
    "--source-antenna",
    type=click.IntRange(0, 7),
    default=0,
    show_default=True,
    help="Native observation input to import into this station; RAWX requires 0.",
)
def run(inputs, station, protocol, rebuild, resume_from, chunk_mib, source_antenna, recursive):
    """Head-probe and time-sort INPUTS, then import one continuous recording path.

    Catalogs: observations (including MeasExtra/smoothing state), raw-bits,
    observation/navigation completion events and measurement-clock evidence.
    RawBits uses a 10-period anchor timeout and 4 MiB backlog. See the coverage
    table for documented and sample-verified adapters. Other telemetry and
    cadence/restart events are not yet imported.
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
        if any(p.is_dir() for p in station.glob("[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]")):
            raise ValueError("Legacy flat date layout: use a newly initialized station; existing products are not migrated")
        if rebuild and resume_from:
            raise ValueError("Rebuild and continuation are mutually exclusive")
        if not inputs:
            raise ValueError("Supply local raw input files")
        new_paths = discover_inputs(inputs, protocol, recursive)
        state = None
        segments = []
        processed = {}
        if not rebuild and resume_from is None:
            for day in reversed(list(day_directories(station))):
                candidate = day / "import-state.json"
                if candidate.exists():
                    candidate_state = json.loads(candidate.read_text(encoding="utf-8"))
                    if not candidate_state.get("continued"):
                        resume_from = candidate
                        break
        if resume_from:
            state = json.loads(resume_from.read_text(encoding="utf-8"))
            if (
                state.get("version") != 3
                or state["protocol"] != protocol
                or state["station"] != str(station)
                or state["source_antenna"] != source_antenna
                or state["setup_id"] != setup["setup_id"]
                or state.get("epoch_period_s") != setup["epoch_period_s"]
                or state.get("continued")
            ):
                raise ValueError("Incompatible or already consumed continuation state")
            click.echo(f"Continuing from {resume_from}", err=True)
            for name in state["published_files"]:
                path = station / name
                if path not in latest_parts(path.parent, parse_name(path.name)[1]):
                    raise ValueError("Continuation state refers to an obsolete revision")
            for item in state["inputs"]:
                path = Path(item["path"])
                if not 0 <= item["offset"] <= item["size"] or path.stat().st_size != item["size"]:
                    raise ValueError(f"Previously read input changed: {path}")
                processed[str(path)] = item
            skipped = sum(str(p) in processed for p in new_paths)
            new_paths = [p for p in new_paths if str(p) not in processed]
            click.echo(f"{skipped} previously read inputs skipped; {len(new_paths)} new inputs", err=True)
            if not new_paths and all(item["offset"] == item["size"] for item in processed.values()):
                click.echo(json.dumps({"status": "up-to-date", "state": str(resume_from)}))
                return
            for item in state["tail_inputs"]:
                path = Path(item["path"])
                if not 0 <= item["offset"] <= item["size"] or path.stat().st_size != item["size"]:
                    raise ValueError("Tail input changed")
                segments.append((path, item["offset"], item["size"]))
        # Saved raw tail is replay context, not a candidate for fresh sorting.
        segments.extend(ordered_inputs(new_paths, protocol))
        mode = "rebuild" if rebuild else "tail" if resume_from else "new"
        stage = Path(tempfile.mkdtemp(prefix=".cnex-import-", dir=station))
        # validate_setup emits exactly twelve fractional digits; avoid both
        # binary floats and Decimal context rounding at this native boundary.
        seconds_text, fraction_text = setup["epoch_period_s"].split(".")
        period_seconds = int(seconds_text)
        if period_seconds > 2**63 - 1:
            raise ValueError("epoch_period_s exceeds native importer range")
        period_ps = int(fraction_text)
        reader = _native.CnexObservationReader(protocol, setup["setup_id"], source_antenna, period_seconds, period_ps)
        if state:
            reader.restore(state["navigation_context"])
        counts = {"observations": 0, "events": 0, "raw-bits": 0, "measurement-clock": 0}
        started = set()
        published = list(state["published_files"]) if state else []
        current_state_path = resume_from

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
                    name = next_name(directory, catalog, "tail" if key in started else mode)
                    started.add(key)
                target = directory / name
                temporary = stage / f"{day_name.replace('/', '-')}-{name}"
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
                writers[key] = (
                    pq.ParquetWriter(
                        temporary, schema, compression="zstd", compression_level=3, use_dictionary=dictionary_columns(schema)
                    ),
                    temporary,
                    target,
                )
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

        def publish(snapshot):
            nonlocal current_state_path
            if rebuild:
                for day_name in {key[0] for key in writers}:
                    for catalog in counts:
                        key = day_name, catalog
                        if key not in started and latest_parts(station / day_name, catalog):
                            get_writer(key, schemas[catalog])
            for writer, _, _ in writers.values():
                if writer is not None:
                    writer.close()
            outputs = closed + [v for v in writers.values() if v[0] is not None]
            for _, _, target in outputs:
                if target.exists():
                    raise ValueError(f"Refusing to overwrite: {target}")
            for _, temporary, target in outputs:
                target.parent.mkdir(parents=True, exist_ok=True)
                temporary.rename(target)
                published.append(str(target.relative_to(station)))
            if snapshot["summary"]["cursor_day"] is None:
                return
            continuation = {
                "version": 3,
                "station": str(station),
                "protocol": protocol,
                "setup_id": setup["setup_id"],
                "source_antenna": source_antenna,
                "epoch_period_s": setup["epoch_period_s"],
                "published_files": published.copy(),
                "tail_inputs": snapshot["tail"],
                "inputs": snapshot["inputs"],
                "continued": False,
                "navigation_context": snapshot["checkpoint"],
            }
            next_state = station / day_path(snapshot["summary"]["cursor_day"]) / "import-state.json"
            next_state.parent.mkdir(parents=True, exist_ok=True)
            write_json(next_state, continuation)
            if current_state_path and current_state_path.resolve() != next_state:
                previous = json.loads(current_state_path.read_text(encoding="utf-8"))
                write_json(current_state_path, {**previous, "continued": True})
            current_state_path = next_state
            if outputs:
                tqdm.write(f"Published {len(outputs)} parts; continuation: {next_state}", file=sys.stderr)
            writers.clear()
            closed.clear()
            active.clear()
            tail_limits.clear()

        def write_batches(batches, snapshot):
            # Only this worker touches catalogs while parsing is running.
            for catalog, batch in zip(counts, batches):
                schemas[catalog] = batch.schema
                if not batch.num_rows:
                    continue
                seconds = pc.cast(batch.column(time_column(catalog)), pa.int64(), safe=False).to_numpy()
                days = seconds // 86400
                for day in np.unique(days):
                    day_name = day_path(day)
                    key = day_name, catalog
                    selected = batch.filter(pa.array(days == day))
                    check_tail(key, selected)
                    get_writer(key, batch.schema).write_batch(selected)
                counts[catalog] += batch.num_rows
            safe_day = snapshot["summary"]["safe_day"]
            if writers and safe_day is not None and min(k[0] for k in writers) < day_path(safe_day):
                publish(snapshot)

        warned_foreign = False
        warned_backlog = False
        total = sum(size - start for _, start, size in segments)
        click.echo(
            "Importing observations, canonical RawBits, completion events and measurement clock evidence.",
            err=True,
        )
        # Bound outstanding work, including the active write, to two input
        # chunks. Arrow buffers are shared, not serialized across the thread.
        pending_writes = deque()
        with (
            ThreadPoolExecutor(max_workers=1, thread_name_prefix="cnex-parquet") as writer_pool,
            tqdm(total=total, unit="B", unit_scale=True, desc="CommonNEX", file=sys.stderr) as progress,
        ):
            for path, start, size in segments:
                with path.open("rb") as source:
                    source.seek(start)
                    remaining = size - start
                    while remaining:
                        if len(pending_writes) >= 2:
                            pending_writes.popleft().result()
                        for future in pending_writes:
                            if future.done():
                                future.result()
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
                        batches = tuple(pa.record_batch(batch) for batch in native_batches)
                        summary = reader.summary()
                        if summary["raw_bits_dropped"] and not warned_backlog:
                            click.echo(
                                "Warning: RawBits backlog exceeded 4 MiB; oldest records discarded (see final counts)", err=True
                            )
                            warned_backlog = True
                        processed[str(path)] = {"path": str(path), "size": size, "offset": size - remaining}
                        tail = []
                        offset = summary["resume_offset"]
                        consumed = summary["source_bytes"]
                        for tail_path, tail_start, tail_size in segments:
                            length = tail_size - tail_start
                            if consumed <= 0:
                                break
                            if offset < length:
                                tail.append({"path": str(tail_path), "offset": tail_start + offset, "size": tail_size})
                                offset = 0
                            else:
                                offset -= length
                            consumed -= length
                        snapshot = {
                            "summary": summary,
                            "checkpoint": reader.checkpoint(),
                            "inputs": list(processed.values()),
                            "tail": tail,
                        }
                        pending_writes.append(writer_pool.submit(write_batches, batches, snapshot))
                        if summary["skipped_protocol_frames"] and not warned_foreign:
                            click.echo("Warning: skipped complete foreign-protocol frames", err=True)
                            warned_foreign = True
                        progress.update(len(data))
                if path.stat().st_size != size:
                    raise ValueError(f"Input changed while reading: {path}")
            for future in pending_writes:
                future.result()
        summary = reader.summary()
        if not started:
            raise ValueError(f"No completed supported records: {summary}")
        publish(snapshot)
        stage.rmdir()
        click.echo(json.dumps({"status": "complete", **counts, **summary, "state": str(current_state_path)}))
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
            click.echo(f"Unpublished staging retained at {stage}; completed daily publications remain available", err=True)
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
