# SPDX-License-Identifier: GPL-3.0-only
"""Preservation-oriented RxTools conversion; source files remain read-only."""

import json
import multiprocessing
import subprocess
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from datetime import datetime
from decimal import Decimal
from pathlib import Path

import click
from tqdm import tqdm

from .gpst import EPOCH
from .research_output import staged_output


def conversion_command(tool, source, version):
    return [str(tool), "-f", str(source), "-o", "copy", "-R" + version.replace(".", ""), "-nOPBM", "-s", "-D", "-X", "-c", "-v"]


def observation_summary(path):
    """Inventory RINEX 3/4 epochs without treating GNSS calendar fields as UTC."""
    header, types, satellites, steps, flags = [], {}, Counter(), Counter(), Counter()
    first = last = previous = None
    epochs = 0
    with Path(path).open() as stream:
        for line in stream:
            header.append(line.rstrip("\n"))
            label = line[60:].strip()
            if label == "SYS / # / OBS TYPES":
                if line[0] != " ":
                    system = line[0]
                    types[system] = []
                types[system].extend(line[7:60].split())
            if label == "END OF HEADER":
                break
        else:
            raise ValueError(f"Missing RINEX header terminator: {path}")
        scale = next((line[48:51].strip() for line in header if line[60:].strip() == "TIME OF FIRST OBS"), None)
        if scale != "GPS":
            raise ValueError(f"Project processing requires explicit GPS observation time, found {scale!r}: {path}")
        for line in stream:
            if line.startswith(">"):
                fields = line[1:].split()
                flag = int(fields[6])
                flags[str(flag)] += 1
                if flag not in (0, 1):
                    for _ in range(int(fields[7])):
                        next(stream)
                    continue
                stamp = " ".join(fields[:6])
                # This integer is only a calendar-difference anchor, not UTC.
                anchor = datetime(*map(int, fields[:5]))
                seconds = Decimal(int((anchor - EPOCH).total_seconds())) + Decimal(fields[5])
                if previous is not None:
                    steps[str((seconds - previous).normalize())] += 1
                previous = seconds
                first = first or stamp
                last = stamp
                epochs += 1
            elif len(line) >= 3 and line[0] in "GRECJSI" and line[1:3].isdigit():
                satellites[line[:3]] += 1
    return dict(
        header=header,
        observation_types=types,
        epochs=epochs,
        first_epoch_calendar=first,
        last_epoch_calendar=last,
        time_system=scale,
        epoch_step_seconds=dict(steps),
        epoch_flags=dict(flags),
        satellite_records=dict(sorted(satellites.items())),
        scope="epoch/header inventory only; not a complete per-observable integrity proof",
    )


def audit_observation(path):
    return dict(path=str(path), summary=observation_summary(path))


@click.command()
@click.option("--obs", multiple=True, required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--output", required=True, type=click.Path(path_type=Path))
@click.option("--workers", type=click.IntRange(1, 32), default=4, show_default=True)
def audit_cli(obs, output, workers):
    """Inventory RINEX 3/4 OBS epochs, signal headers and satellites."""
    try:
        with output.open("x") as stream:
            with ProcessPoolExecutor(max_workers=min(workers, len(obs)), mp_context=multiprocessing.get_context("spawn")) as pool:
                reports = list(tqdm(pool.map(audit_observation, [p.resolve() for p in obs]), total=len(obs), desc="Audit RINEX"))
            json.dump(dict(files=reports), stream, indent=2)
            stream.write("\n")
        click.echo(json.dumps(dict(status="complete", output=str(output))))
    except (OSError, ValueError) as error:
        raise click.ClickException(str(error)) from error


def convert(job):
    source, output, tool, version, tool_version = job
    output.mkdir()
    command = conversion_command(tool, source, version)
    report = dict(
        status="running",
        source=str(source),
        source_bytes=source.stat().st_size,
        tool=str(tool),
        tool_version=tool_version,
        command=command,
        cwd=".",
        policy=dict(
            rinex_version=version,
            interval="native; no resampling",
            signals="all supported; no include/exclude filters",
            antenna=1,
            requested_outputs=["observations", "mixed_navigation", "SBAS_broadcast", "meteorology"],
            extra_observables=["S", "D", "X1"],
            comments=True,
            external_events=True,
            time_scale="GPST",
            partition="input-file boundary; validate GPST payload coverage before canonical publication",
            continuity="independent file conversion; cross-file continuity not yet validated",
        ),
    )
    manifest = output / "conversion.json"
    manifest.write_text(json.dumps(report, indent=2) + "\n")
    started = time.monotonic()
    with (output / "converter.log").open("xb") as log:
        result = subprocess.run(command, cwd=output, stdout=log, stderr=subprocess.STDOUT)
    artifacts = [
        dict(path=p.name, bytes=p.stat().st_size)
        for p in sorted(output.iterdir())
        if p.name not in ("conversion.json", "converter.log") and p.is_file()
    ]
    report.update(
        status="complete" if result.returncode == 0 and artifacts else "failed",
        returncode=result.returncode,
        elapsed_seconds=time.monotonic() - started,
        artifacts=artifacts,
    )
    if report["status"] == "complete":
        try:
            report["observation_summary"] = {
                item["path"]: observation_summary(output / item["path"])
                for item in artifacts
                if Path(item["path"]).suffix.lower().endswith("o")
            }
            if not report["observation_summary"]:
                raise ValueError("No GPS-time observation file generated")
        except ValueError as error:
            report.update(status="failed", validation_error=str(error))
    manifest.write_text(json.dumps(report, indent=2) + "\n")
    if report["status"] != "complete":
        raise ValueError(f"Conversion failed; see {output / 'converter.log'}")
    return dict(source=str(source), output=output.name, artifacts=len(artifacts))


@click.command()
@click.option("--source", multiple=True, required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option(
    "--tool", required=True, type=click.Path(exists=True, dir_okay=False, path_type=Path), help="Installed RxTools sbf2rin."
)
@click.option(
    "--output", required=True, type=click.Path(path_type=Path), help="New output directory; existing paths are never reused."
)
@click.option("--rinex-version", type=click.Choice(["3.04", "3.05", "4.00", "4.01"]), default="4.01", show_default=True)
@click.option("--workers", type=click.IntRange(1, 32), default=4, show_default=True)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(source, tool, output, rinex_version, workers):
    """Convert expanded SBF files without resampling or signal filtering."""
    try:
        sources = [p.resolve() for p in source]
        if len(set(sources)) != len(sources):
            raise ValueError("Duplicate input paths")
        if any(p.suffix.lower() not in (".sbf", ".25_") for p in sources):
            raise ValueError("Expected expanded .sbf or .25_ inputs; compressed inputs are not accepted")
        tool, output = tool.resolve(), output.resolve()
        version = subprocess.check_output([str(tool), "-V"], text=True, stderr=subprocess.STDOUT).strip()
        output.mkdir(parents=True, exist_ok=False)
        jobs = [(p, output / f"source-{i:05d}", tool, rinex_version, version) for i, p in enumerate(sources)]
        # The CPU-heavy work runs in separate native processes; threads only orchestrate them.
        with ThreadPoolExecutor(max_workers=min(workers, len(jobs))) as pool:
            results = list(tqdm(pool.map(convert, jobs), total=len(jobs), desc="SBF to RINEX", unit="file"))
        (output / "completed.json").write_text(json.dumps(dict(status="complete", groups=results), indent=2) + "\n")
        click.echo(json.dumps(dict(status="complete", files=len(results))))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
