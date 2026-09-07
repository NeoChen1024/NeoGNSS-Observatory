# SPDX-License-Identifier: GPL-3.0-only
"""Extract reconstructed SBAS streams without resetting at continuous file boundaries."""

import hashlib
import json
import os
import subprocess
from collections import Counter
from pathlib import Path

import click
from tqdm import tqdm

from .research_output import staged_output


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


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


def extract_group(root, group, output, worker, progress):
    # One persistent native reader/parser for the whole continuous byte stream.
    # The pipe does not reach EOF at an intermediate source-file boundary.
    with tempfile_logs(output.parent, output.name) as logs:
        process = subprocess.Popen(
            [str(worker), "/dev/stdin", str(output), "--sbas-only"], stdin=subprocess.PIPE, stdout=logs[0], stderr=logs[1]
        )
        sources = []
        stream_offset = 0
        try:
            for source in group:
                path = root / source["name"]
                size = 0
                with path.open("rb") as stream:
                    before = os.fstat(stream.fileno())
                    if (before.st_size, before.st_mtime_ns) != (source["size"], source["mtime_ns"]):
                        raise ValueError(f"Source changed: {path}")
                    while block := stream.read(1024 * 1024):
                        process.stdin.write(block)
                        size += len(block)
                        progress.update(len(block))
                    after = os.fstat(stream.fileno())
                if size != source["size"] or before.st_mtime_ns != after.st_mtime_ns:
                    raise ValueError(f"Source verification failed: {path}")
                sources.append(dict(source, stream_begin=stream_offset, stream_end=stream_offset + size))
                stream_offset += size
            process.stdin.close()
            if process.wait() != 0:
                raise ValueError(f"Native worker failed; inspect {output.name}.stderr")
        except BaseException:
            process.terminate()
            process.wait()
            raise
        finally:
            if not process.stdin.closed:
                process.stdin.close()
    summary = json.loads((output / "summary.json").read_text())
    if summary["source_bytes"] != stream_offset:
        raise ValueError("Worker consumed an unexpected byte count")
    # The native summary describes /dev/stdin. This mapping identifies every
    # original source byte, without assigning a fabricated SFRBX timestamp.
    write_json(
        output / "sources.json",
        {
            "sources": sources,
            "time_basis": "reconstruction_payload_gpst",
            "start_gpst": group[0]["start_gpst"],
            "end_gpst": group[-1]["end_gpst"],
        },
    )
    return summary


class tempfile_logs:
    """Retain diagnostic logs even if extraction is interrupted."""

    def __init__(self, directory, name):
        self.paths = [directory / (name + suffix) for suffix in (".stdout", ".stderr")]

    def __enter__(self):
        self.streams = [path.open("xb") for path in self.paths]
        return self.streams

    def __exit__(self, *_):
        for stream in self.streams:
            stream.close()


@click.command()
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="New output directory; never overwrites a run.")
@click.option("--worker", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, worker):
    """Extract all SBAS from reconstructed GPST segments, excluding unassigned/."""
    try:
        root, worker = input_dir.resolve(), worker.resolve()
        segments, completed = inventory(root)
        groups = continuous_groups(segments)
        output.mkdir(parents=False, exist_ok=False)
        write_json(output / "run.json", {"time_scale": "GPST", "source_root": str(root)})
        totals, types, statuses, satellites = Counter(), Counter(), Counter(), Counter()
        click.echo(
            f"{len(segments)} segments; {len(groups)} continuous groups; {len(segments)-len(groups)} joined boundaries", err=True
        )
        with (
            (output / "groups.jsonl").open("x") as journal,
            tqdm(total=sum(s["size"] for s in segments), desc="Extract SBAS", unit="B", unit_scale=True, mininterval=5) as progress,
        ):
            for number, group in enumerate(groups):
                name = f"group-{number:05d}"
                summary = extract_group(root, group, output / name, worker, progress)
                for key in ("source_bytes", "ubx_frames", "malformed", "discarded_noise_bytes"):
                    totals[key] += summary[key]
                statuses.update(summary["sbas_status"])
                types.update(summary["sbas_message_types"])
                for stream in summary["streams"]:
                    satellites[str(stream["svId"])] += stream["frames"]
                journal.write(json.dumps({"group": name, "files": len(group), "summary": summary}) + "\n")
                journal.flush()
                progress.set_postfix(groups=f"{number+1}/{len(groups)}", sbas=sum(statuses.values()), refresh=False)
        result = dict(
            status="complete",
            segments=len(segments),
            continuous_groups=len(groups),
            **totals,
            sbas_frames=sum(statuses.values()),
            sbas_status=dict(statuses),
            sbas_message_types=dict(types),
            sbas_prns=dict(satellites),
        )
        write_json(output / "completed.json", result)
        click.echo(json.dumps(result))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
