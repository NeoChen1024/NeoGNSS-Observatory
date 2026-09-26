#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""JSONL adapter for the shared CommonNEX SBAS snapshot processor."""

import json
from contextlib import nullcontext
from pathlib import Path

import click

from .broadcast_realtime import json_value
from .cnex_stream import CnexStream, file_groups, tcp_groups
from .sbas_grid import SbasGridProcessor
from .setup_metadata import validate_setup


def snapshot_records(output):
    snapshots = {}
    for kind, batches in output.items():
        for batch in batches:
            for row in batch.to_pylist():
                stamp = row["snapshot_gpst"]
                record = snapshots.setdefault(stamp, dict(type="snapshot", snapshot_gpst=stamp, grid=[]))
                if kind == "snapshots":
                    record.update(row)
                else:
                    record["grid"].append(row)
    yield from (snapshots[t] for t in sorted(snapshots))


@click.command()
@click.option("--host", help="Receiver TCP host or unbracketed IPv6 address.")
@click.option("--port", type=click.IntRange(1, 65535), default=2006, show_default=True)
@click.option("--input", "input_path", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("-p", "--protocol", type=click.Choice(["ubx", "sbf"]), required=True)
@click.option("--setup", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--snapshot-interval", type=click.IntRange(1), default=3600, show_default=True)
@click.option("--correction-age", type=click.IntRange(1), default=600, show_default=True)
@click.option("--mask-age", type=click.IntRange(1), default=1200, show_default=True)
@click.option("--max-latency", type=click.FloatRange(min=0, min_open=True), default=5.0, show_default=True)
@click.option("--duration", type=click.FloatRange(min=0, min_open=True), help="TCP capture seconds; default unlimited.")
@click.option("--output", type=click.Path(path_type=Path), help="Exclusive-create JSONL file; default stdout.")
def cli(host, port, input_path, protocol, setup, snapshot_interval, correction_age, mask_age, max_latency, duration, output):
    """Emit per-source SBAS snapshots using the shared CommonNEX normalizer."""
    if bool(host) == bool(input_path):
        raise click.UsageError("Specify exactly one of --host or --input")
    if input_path and duration is not None:
        raise click.UsageError("--duration applies only to TCP")
    try:
        metadata = validate_setup(json.loads(setup.read_text()))
        sec, fraction = metadata["epoch_period_s"].split(".")
        stream = CnexStream(protocol, metadata["setup_id"], int(sec), int(fraction))
        processor = SbasGridProcessor(metadata["setup_id"], snapshot_interval, correction_age, mask_age)
        groups = (
            file_groups(input_path, stream)
            if input_path
            else tcp_groups(host, port, stream, max_latency=max_latency, duration=duration)
        )
        with output.open("x") if output else nullcontext(click.get_text_stream("stdout")) as target:
            for group in groups:
                if group is None:
                    continue
                for record in snapshot_records(processor.feed(group)):
                    target.write(json.dumps(record, default=json_value, allow_nan=False) + "\n")
                target.flush()
                if group.notice:
                    click.echo(group.notice, err=True)
            processor.finish()
        click.echo(json.dumps(processor.diagnostics, sort_keys=True), err=True)
    except (OSError, ValueError, RuntimeError, KeyError, BufferError, EOFError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
