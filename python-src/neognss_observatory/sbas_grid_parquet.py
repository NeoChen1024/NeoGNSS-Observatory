# SPDX-License-Identifier: GPL-3.0-only
"""Produce daily GPST SBAS grid intervals directly from reconstructed UBX."""

import json
import subprocess
import sys
from collections import Counter, defaultdict
from contextlib import redirect_stdout
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import sbas_extract
from .gpst import label
from .research_output import staged_output
from .sbas_grid import COORDINATES, TECU_PER_M, HourlyGrid
from .sbas_grid_render import EraATimeMapper, GroupOffsetMapper, group_paths, write_json

SCHEMA = pa.schema(
    [
        (name, pa.int64())
        for name in (
            "start_gpst_ms",
            "end_gpst_ms",
            "gnssId",
            "svId",
            "sigId",
            "freqId",
            "band",
            "mask_bit",
            "iodi",
            "givei",
            "frame_offset",
        )
    ]
    + [(name, pa.float64()) for name in ("latitude", "longitude", "delay_m", "vtec_tecu")]
    + [("group", pa.string())],
    metadata={
        b"schema_version": b"1",
        b"time_scale": b"GPST",
        b"time_origin": b"1980-01-06 00:00:00 GPST",
        b"time_basis": b"NAV/EOE reception context, not SBAS transmit time",
        b"interval": b"[start_gpst_ms,end_gpst_ms)",
    },
)


class IntervalGrid(HourlyGrid):
    """Emit valid sample-and-hold intervals without retaining hourly history."""

    def __init__(self, emit):
        super().__init__()
        self.emit = emit
        self.references = {}

    def flush_cell(self, key, time):
        start, expiry, value = self.cells[key]
        end = min(time, expiry, self.mask_deadline())
        if end > start:
            lat, lon = COORDINATES[key]
            self.emit(
                dict(
                    start_gpst_ms=round(start * 1000),
                    end_gpst_ms=round(end * 1000),
                    band=key[0],
                    mask_bit=key[1],
                    latitude=lat,
                    longitude=lon,
                    vtec_tecu=value,
                    delay_m=value / TECU_PER_M,
                    **self.references[key],
                )
            )
        self.cells[key] = (time, expiry, value)

    def accept(self, time, message, offset):
        super().process(time, message)
        if message.get("status") == "decoded" and message["type"] == 26:
            content = message["content"]
            band = content["band"]
            if band in self.masks and content["iodi"] == self.iodi and self.mask_deadline() > time:
                positions = self.masks[band][0]
                for i, correction in enumerate(content["corrections"]):
                    ordinal = content["block"] * 15 + i
                    if ordinal < len(positions):
                        key = band, positions[ordinal]
                        if (
                            key in self.cells
                            and self.cells[key][0] == time
                            and correction["status"] == "usable"
                            and correction["givei"] != 15
                            and correction["delay_raw"] != 511
                        ):
                            self.references[key] = dict(iodi=self.iodi, givei=correction["givei"], frame_offset=offset)


class DailySink:
    """Bounded buffers, followed by streaming compaction to one file per day."""

    def __init__(self, output):
        self.output = output
        self.staging = output / "staging"
        self.staging.mkdir()
        self.buffer = defaultdict(list)
        self.count = 0
        self.parts = defaultdict(list)

    def add(self, row):
        start, end = row["start_gpst_ms"], row["end_gpst_ms"]
        while start < end:
            day = start // 86400000 * 86400000
            stop = min(end, day + 86400000)
            self.buffer[day].append(dict(row, start_gpst_ms=start, end_gpst_ms=stop))
            self.count += 1
            start = stop
        if self.count >= 8192:
            self.flush()

    def flush(self):
        for day, rows in self.buffer.items():
            path = self.staging / f"{day}-{len(self.parts[day])}.parquet"
            pq.write_table(pa.Table.from_pylist(rows, schema=SCHEMA), path, compression="zstd", compression_level=3)
            self.parts[day].append(path)
        self.buffer.clear()
        self.count = 0

    def finish(self):
        self.flush()
        directory = self.output / "daily"
        directory.mkdir()
        manifest = []
        for day, parts in tqdm(sorted(self.parts.items()), desc="Write GPST days", unit="day"):
            path = directory / (label(day // 1000)[:15] + ".parquet")
            count = 0
            schema = SCHEMA.with_metadata(
                {
                    **SCHEMA.metadata,
                    b"day_gpst_ms": str(day).encode(),
                    b"correction_age_seconds": b"600",
                    b"mask_age_seconds": b"1200",
                    b"tail_policy": b"stop at final observed epoch; no extrapolation",
                }
            )
            with pq.ParquetWriter(path, schema, compression="zstd", compression_level=3) as writer:
                for part in parts:
                    for batch in pq.ParquetFile(part).iter_batches():
                        writer.write_table(pa.Table.from_batches([batch]).replace_schema_metadata(schema.metadata))
                        count += batch.num_rows
            manifest.append(dict(path=str(path.relative_to(self.output)), day_gpst_ms=day, rows=count))
            for part in parts:
                part.unlink()  # Only temporary shards created by this sink.
        self.staging.rmdir()
        return manifest


def build_grid(extraction, reconstruction, output):
    paths = list(group_paths(extraction))
    sink = DailySink(output)
    mapper = EraATimeMapper(reconstruction)
    diagnostics = Counter()
    try:
        for group in tqdm(paths, desc="SBAS grid state", unit="group"):
            signal_paths = sorted(group.glob("gnss-1_*.jsonl"))
            source_info = json.loads((group / "sources.json").read_text())
            for path in signal_paths:
                offsets = GroupOffsetMapper(mapper, source_info["sources"])
                states = {}
                with path.open() as stream:
                    for line in stream:
                        record = json.loads(line)
                        message = record["sbas"]
                        if message.get("type") not in (0, 18, 26):
                            continue
                        key = tuple(record[k] for k in ("gnssId", "svId", "sigId", "freqId"))
                        if key not in states:
                            identity = dict(zip(("gnssId", "svId", "sigId", "freqId"), key), group=group.name)
                            states[key] = IntervalGrid(lambda row, identity=identity: sink.add(dict(row, **identity)))
                        states[key].accept(offsets.gpst(record["offset"]), message, record["offset"])
                for state in states.values():
                    # No fabricated extra second or assumed receiver cadence at EOF.
                    state.finish(source_info["end_gpst"])
                    diagnostics.update(state.diagnostics)
    finally:
        mapper.close()
    return sink.finish(), dict(diagnostics)


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option(
    "--worker", type=click.Path(exists=True, dir_okay=False, path_type=Path), help="Required unless --sbas-dir is supplied."
)
@click.option(
    "--sbas-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    help="Reuse a completed extraction; skip reading UBX. Output must still be new.",
)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.pass_context
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(context, input_dir, worker, output, sbas_dir=None):
    """Parse reconstructed UBX into daily GPST SBAS grid interval Parquet."""
    try:
        input_dir = input_dir.resolve()
        if sbas_dir is not None:
            if worker is not None:
                raise ValueError("Use either --worker or --sbas-dir, not both")
            extraction = sbas_dir.resolve()
        elif worker is None:
            raise ValueError("--worker is required unless --sbas-dir is supplied")
        output.mkdir(parents=False, exist_ok=False)
        if sbas_dir is None:
            extraction = output / "frames"
            with redirect_stdout(sys.stderr):
                context.invoke(sbas_extract.cli, input_dir=input_dir, worker=worker, output=extraction)
        manifest, diagnostics = build_grid(extraction, input_dir, output)
        write_json(
            output / "completed.json",
            dict(
                schema=1,
                status="complete",
                time_scale="GPST",
                time_origin="1980-01-06 00:00:00 GPST",
                product="sbas_igp_intervals",
                files=manifest,
                diagnostics=diagnostics,
                policy=dict(
                    correction_age_seconds=600,
                    mask_age_seconds=1200,
                    tail="stop at final observed epoch; no cadence extrapolation",
                    invalid="excluded from intervals; retained in frames",
                    gaps="reset at reconstruction continuous-group boundaries",
                ),
                extraction_root="frames" if sbas_dir is None else str(extraction.resolve()),
            ),
        )
        click.echo(json.dumps(dict(status="complete", days=len(manifest), intervals=sum(r["rows"] for r in manifest))))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
