# SPDX-License-Identifier: GPL-3.0-only
"""Extract reconstructed SBAS streams without resetting at continuous file boundaries."""

import hashlib
import json
import os
from collections import Counter
from pathlib import Path

import click
from tqdm import tqdm

from . import _native
from .protocol import ProtocolWarnings, protocol_option
from .research_output import staged_output, write_json


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory(root):
    completed = json.loads((root / "completed.json").read_text())
    if completed.get("status") != "complete":
        raise ValueError("Reconstruction is not complete")
    metadata = {}
    for path in (root / ".artifacts").glob("*.json"):
        record = json.loads(path.read_text())
        if record.get("kind") == "gpst_segment":
            if record["name"] in metadata:
                raise ValueError("Duplicate artifact metadata")
            metadata[record["name"]] = record
    segments = []
    with (root / "plan.jsonl").open() as stream:
        for line in stream:
            record = json.loads(line)
            if record.get("record_type") != "artifacts" or record["kind"] != "gpst_segment":
                continue
            name = record["name"]
            if Path(name).name != name or not name.endswith(".ubx"):
                raise ValueError("Expected root-level GPST segment")
            meta = metadata[name]
            if not meta.get("verified") or any(meta.get(k) != v for k, v in record.items() if k != "record_type"):
                raise ValueError(f"Artifact metadata does not match plan: {name}")
            stat = (root / name).stat()
            if stat.st_size != meta["size"]:
                raise ValueError(f"Source size changed: {name}")
            if "start_gpst_ms" in meta:
                start, end = meta["start_gpst_ms"], meta["end_gpst_ms"]
                count, maximum = meta["nav_epochs"], meta["max_observed_interval_ms"]
                if (
                    start / 1000 != meta["start_gpst"]
                    or end / 1000 != meta["end_gpst"]
                    or count < 1
                    or end < start
                    or not 0 <= maximum <= meta["gap_timeout_ms"]
                    or (count == 1 and (start != end or maximum != 0))
                    or (count > 1 and not count - 1 <= end - start <= (count - 1) * maximum)
                ):
                    raise ValueError(f"Invalid millisecond segment coverage: {name}")
            elif meta["end_gpst"] - meta["start_gpst"] + 1 != meta["nav_seconds"]:
                raise ValueError(f"Non-contiguous segment: {name}")
            segments.append({k: meta[k] for k in ("name", "size", "sha256", "start_gpst", "end_gpst")})
            segments[-1]["mtime_ns"] = stat.st_mtime_ns
            if "start_gpst_ms" in meta:
                segments[-1].update({k: meta[k] for k in ("start_gpst_ms", "end_gpst_ms", "gap_timeout_ms")})
    if len(segments) != completed["gpst_segments"] or {p.name for p in root.glob("*.ubx")} != {s["name"] for s in segments}:
        raise ValueError("Segment inventory does not match reconstruction")
    return segments, completed


def continuous_groups(segments):
    groups = []
    for segment in sorted(segments, key=lambda s: s["start_gpst"]):
        if groups and segment["start_gpst"] <= groups[-1][-1]["end_gpst"]:
            raise ValueError("Overlapping or reversed segment timestamps")
        timeout = min(segment.get("gap_timeout_ms", 1000), groups[-1][-1].get("gap_timeout_ms", 1000)) if groups else 1000
        if not groups or round((segment["start_gpst"] - groups[-1][-1]["end_gpst"]) * 1000) > timeout:
            groups.append([])
        groups[-1].append(segment)
    return groups


def extract_ubx(root, sink):
    from .sbas_frames import FrameStreams
    from .ubx_sbas_time import EraATimeMapper, GroupOffsetMapper

    segments, _ = inventory(root)
    groups = continuous_groups(segments)
    streams = FrameStreams(sink)
    mapper = EraATimeMapper(root)
    totals = Counter()
    try:
        with tqdm(total=sum(s["size"] for s in segments), desc="Extract SBAS", unit="B", unit_scale=True) as progress:
            for group in groups:
                sources, offset = [], 0
                for source in group:
                    sources.append(dict(source, stream_begin=offset, stream_end=offset + source["size"]))
                    offset += source["size"]
                times = GroupOffsetMapper(mapper, sources)
                parser = _native.SubframeProcessor(sbas_only=True)

                def consume(rows):
                    for row in rows:
                        if (row["gnssId"], row["sigId"], row["freqId"]) != (1, 0, 0):
                            continue
                        if "hex" not in row["sbas"]:
                            totals["incomplete_sbas"] += 1
                            continue
                        streams.add(row["prn"], round(times.gpst(row["offset"]) * 1000), row["sbas"], "navigation_epoch_context")

                for source in group:
                    path = root / source["name"]
                    warnings = ProtocolWarnings(path, "ubx", parser.summary())
                    with path.open("rb") as stream:
                        before = os.fstat(stream.fileno())
                        if (before.st_size, before.st_mtime_ns) != (source["size"], source["mtime_ns"]):
                            raise ValueError(f"Source changed: {path}")
                        while block := stream.read(4 * 1024 * 1024):
                            consume(parser.feed(block))
                            warnings.update(parser.summary())
                            progress.update(len(block))
                        after = os.fstat(stream.fileno())
                    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                        raise ValueError(f"Source changed: {path}")
                    warnings.update(parser.summary(), final=True)
                consume(parser.finish())
                streams.finish(round(group[-1]["end_gpst"] * 1000))
                totals.update({k: v for k, v in parser.summary().items() if isinstance(v, int)})
    finally:
        mapper.close()
    return dict(totals)


@click.command()
@protocol_option
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option(
    "--gap-timeout",
    type=click.FloatRange(min=0.001),
    default=50,
    show_default=True,
    help="SBF per-signal gap in seconds; UBX uses reconstruction groups.",
)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, protocol="ubx", gap_timeout=50):
    """Extract source-independent SBAS frame Parquet, partitioned by GPST day."""
    from .sbas_frames import FrameSink

    try:
        output.mkdir()
        sink = FrameSink(output)
        if protocol == "sbf":
            from .sbf_extract import extract

            diagnostics = extract(input_dir.resolve(), sink, round(gap_timeout * 1000))
        else:
            diagnostics = extract_ubx(input_dir.resolve(), sink)
        days = sink.finish()
        result = dict(status="complete", product="sbas_frames", days=days, frames=sink.frames, diagnostics=diagnostics)
        write_json(output / "completed.json", result)
        click.echo(json.dumps(result))
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
