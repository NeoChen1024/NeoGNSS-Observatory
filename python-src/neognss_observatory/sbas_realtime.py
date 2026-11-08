#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""JSONL adapter for the shared CommonNEX SBAS snapshot processor."""

import json

import click

from .broadcast_realtime import json_value
from .cnex_stream import LIVE_ERRORS, live_options, live_source, write_jsonl
from .sbas_grid import SbasGridProcessor


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
@live_options
@click.option("--snapshot-interval", type=click.IntRange(1), default=3600, show_default=True)
@click.option("--correction-age", type=click.IntRange(1), default=600, show_default=True)
@click.option("--mask-age", type=click.IntRange(1), default=1200, show_default=True)
def cli(host, port, input_path, protocol, setup_path, max_latency, duration, output, snapshot_interval, correction_age, mask_age):
    """Emit per-source SBAS snapshots using the shared CommonNEX normalizer."""
    try:
        setup, groups = live_source(host, port, input_path, protocol, setup_path, max_latency, duration)
        processor = SbasGridProcessor(setup["setup_id"], snapshot_interval, correction_age, mask_age)
        write_jsonl(groups, lambda group: snapshot_records(processor.feed(group)), output, json_value)
        processor.finish()
        click.echo(json.dumps(processor.diagnostics, sort_keys=True), err=True)
    except LIVE_ERRORS as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
