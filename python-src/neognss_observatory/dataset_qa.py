# SPDX-License-Identifier: GPL-3.0-only
"""Optional dataset QA and overlap reconstruction profiles."""

import json
from pathlib import Path

import click
from tqdm import tqdm

from . import _native
from .dataset_inputs import recordings
from .protocol import ProtocolWarnings, protocol_option


@click.command()
@protocol_option
@click.option("--profile", type=click.Choice(["scan", "restitch"]), default="scan", show_default=True)
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--recursive/--no-recursive", default=True, show_default=True)
@click.option("--gap-timeout", type=click.FloatRange(min=0.001), default=50, show_default=True)
@click.option("--state-dir", type=click.Path(file_okay=False, path_type=Path), help="Restitch-only index/cache directory.")
@click.option("--output-dir", type=click.Path(file_okay=False, path_type=Path), help="Restitch-only reconstructed recordings.")
@click.option("--plan-only", is_flag=True, help="Restitch-only: write a plan without publishing recordings.")
@click.pass_context
def cli(context, protocol, profile, input_dir, recursive, gap_timeout, state_dir, output_dir, plan_only):
    """Scan read-only by default; reconstruct only when explicitly selected."""
    try:
        if profile == "restitch":
            if protocol != "ubx":
                raise ValueError("Overlap reconstruction currently supports UBX only; SBF supports scan")
            if state_dir is None or output_dir is None:
                raise ValueError("Restitch requires --state-dir and --output-dir")
            from .ubx_restitch import run_restitch

            return run_restitch(
                input_dir=input_dir,
                state_dir=state_dir,
                recursive=recursive,
                output_dir=output_dir,
                plan_only=plan_only,
                gap_timeout=gap_timeout,
                protocol=protocol,
            )
        if state_dir is not None or output_dir is not None or plan_only:
            raise ValueError("Scan is read-only; state/output/plan options apply only to --profile restitch")
        paths = recordings(input_dir.resolve(), protocol, recursive)
        scanner = _native.DatasetScan(protocol=protocol, qa=True, gap_timeout=gap_timeout)
        with tqdm(total=sum(p.stat().st_size for p in paths), desc="Dataset QA", unit="B", unit_scale=True) as progress:
            for path in paths:
                warnings = ProtocolWarnings(path, protocol, scanner.summary())
                with path.open("rb") as source:
                    while data := source.read(4 * 1024 * 1024):
                        scanner.feed(data)
                        warnings.update(scanner.summary())
                        progress.update(len(data))
                warnings.update(scanner.summary(), final=True)
        scanner.finish()
        summary = scanner.summary()
        findings = (
            summary["invalid_frames"]
            + summary["noise_bytes"]
            + sum(
                summary["qa"].get(k, 0)
                for k in (
                    "time_reversals",
                    "duplicate_epochs",
                    "duplicate_epoch_blocks",
                    "time_conflicts",
                    "invalid_payloads",
                    "invalid_times",
                    "truncated_tail",
                    "untimed_epochs",
                )
            )
        )
        click.echo(json.dumps(dict(status="findings" if findings else "complete", profile=profile, files=len(paths), **summary)))
        if findings:
            context.exit(1)
    except (OSError, ValueError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
