#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Typed canonical broadcast decoding; JSONL is only the example CLI output."""

import json
from contextlib import nullcontext
from decimal import Decimal
from pathlib import Path

import click
import pyarrow as pa
import pyarrow.compute as pc

from . import _native
from .cnex_stream import CnexStream, file_groups, tcp_groups
from .setup_metadata import validate_setup


def aggregate_almanacs(output):
    """Merge equal snapshot candidates only; MessageOutput stays source-local."""
    batches = output.pop("snapshot_almanac_entry", [])
    if not batches:
        return output
    excluded = {
        "broadcasting_satellite",
        "bitstream_source",
        "nav_epoch_gpst",
        "first_received_gpst",
        "output_sequence",
        "subframe_id",
        "tow_count",
        "alert",
        "antispoof",
    }
    columns = [field for field in batches[0].schema if field.name not in excluded]
    source_type = pa.struct(
        [
            ("broadcasting_satellite", pa.int64()),
            ("bitstream_source", pa.list_(pa.string())),
            ("first_received_gpst", pa.decimal128(38, 12)),
            ("last_received_gpst", pa.decimal128(38, 12)),
        ]
    )
    schema = pa.schema(columns + [pa.field("sources", pa.list_(source_type)), pa.field("conflict_present", pa.bool_())])
    candidates = {}
    for batch in batches:
        for row in batch.to_pylist():
            parameters = {f.name: row[f.name] for f in columns}
            # No cross-source aggregation without an established reference epoch.
            key = tuple(parameters.items()) + (
                ()
                if row["reference_gpst"] is not None
                else (("unresolved_source", (row["broadcasting_satellite"], tuple(row["bitstream_source"]))),)
            )
            candidate = candidates.setdefault(key, {**parameters, "sources": [], "conflict_present": False})
            candidate["sources"].append(
                {
                    "broadcasting_satellite": row["broadcasting_satellite"],
                    "bitstream_source": row["bitstream_source"],
                    "first_received_gpst": row["first_received_gpst"],
                    "last_received_gpst": row["nav_epoch_gpst"],
                }
            )
    identities = {}
    for row in candidates.values():
        key = tuple(row[k] for k in ("snapshot_gpst", "satellite_system", "message_family", "reference_gpst", "subject_sv_id"))
        identities.setdefault(key, []).append(row)
    for key, rows in identities.items():
        if key[3] is not None and len(rows) > 1:
            for row in rows:
                row["conflict_present"] = True
    output["snapshot_almanac_entries"] = [pa.RecordBatch.from_pylist(list(candidates.values()), schema=schema)]
    sets = {}
    for row in candidates.values():
        key = tuple(row[k] for k in ("snapshot_gpst", "satellite_system", "message_family", "reference_gpst"))
        item = sets.setdefault(key, {"members": set(), "conflict": False})
        item["members"].add(row["subject_sv_id"])
        item["conflict"] |= row["conflict_present"]
    set_schema = pa.schema(
        [
            ("snapshot_gpst", pa.decimal128(38, 12)),
            ("satellite_system", pa.string()),
            ("message_family", pa.string()),
            ("reference_gpst", pa.decimal128(38, 12)),
            ("received_satellite_count", pa.int64()),
            ("expected_satellite_count", pa.int64()),
            ("completeness", pa.string()),
            ("conflict_present", pa.bool_()),
        ]
    )
    output["snapshot_almanac_sets"] = [
        pa.RecordBatch.from_pylist(
            [
                dict(
                    zip(("snapshot_gpst", "satellite_system", "message_family", "reference_gpst"), key),
                    received_satellite_count=len(value["members"]),
                    expected_satellite_count=None,
                    completeness="UNKNOWN",
                    conflict_present=value["conflict"],
                )
                for key, value in sets.items()
            ],
            schema=set_schema,
        )
    ]
    return output


class BroadcastMessageDecoder:
    """One source-ordered consumer. Native output categories are Arrow batches."""

    def __init__(self, setup_id, snapshot_interval_s=3600):
        self.native = _native.BroadcastMessageDecoder(setup_id, snapshot_interval_s)

    def process(self, raw_bits):
        if isinstance(raw_bits, pa.Table):
            batches = raw_bits.to_batches()
        elif isinstance(raw_bits, pa.RecordBatch):
            batches = (raw_bits,)
        else:
            batches = raw_bits
        output = {}
        for batch in batches:
            for kind, value in self.native.feed(batch).items():
                output.setdefault(kind, []).append(pa.record_batch(value))
        return aggregate_almanacs(output)

    def feed(self, group):
        resets = []
        for batch in group.catalogs.get("events", ()):
            selected = batch.filter(pc.equal(batch.column("kind"), "RECEIVER_RESTART"))
            resets.extend(selected.column("gpst").to_pylist())
        if any(t is None for t in resets):
            raise ValueError("Untimed receiver restart cannot be ordered for broadcast decoding")
        resets.sort()
        result = {}

        def consume(batch):
            for kind, values in self.process(batch).items():
                result.setdefault(kind, []).extend(values)

        for batch in group.catalogs.get("raw-bits", ()):
            if not resets:
                consume(batch)
                continue
            stamps = batch.column("nav_epoch_gpst").to_pylist()
            begin = 0
            for i, stamp in enumerate(stamps):
                while resets and stamp is not None and resets[0] <= stamp:
                    if i > begin:
                        consume(batch.slice(begin, i - begin))
                    self.native.discontinuity()
                    resets.pop(0)
                    begin = i
            if begin < batch.num_rows:
                consume(batch.slice(begin))
        for _ in resets:
            self.native.discontinuity()
        if group.notice:
            self.native.discontinuity()
        return result

    @property
    def diagnostics(self):
        return self.native.diagnostics


def json_value(value):
    if isinstance(value, Decimal):
        return format(value, "f")
    if isinstance(value, bytes):
        return value.hex()
    raise TypeError(f"Unsupported JSON value: {type(value).__name__}")


def json_records(outputs):
    """Expose per-message records and full snapshot bundles, without raw frames."""
    records, snapshots = [], {}
    for kind, batches in outputs.items():
        for batch in batches:
            for row in batch.to_pylist():
                if kind == "snapshot":
                    stamp = row.pop("snapshot_gpst")
                    snapshot = snapshots.setdefault(stamp, {"type": "snapshot", "snapshot_gpst": stamp, "catalogs": {}})
                    snapshot.update(row)
                elif kind.startswith("snapshot_"):
                    stamp = row.pop("snapshot_gpst")
                    snapshot = snapshots.setdefault(stamp, {"type": "snapshot", "snapshot_gpst": stamp, "catalogs": {}})
                    snapshot["catalogs"].setdefault(kind.removeprefix("snapshot_"), []).append(row)
                else:
                    records.append({"type": "message", "kind": kind, **row})
    # Same-context messages follow snapshots at that grid point. Untimed records
    # carry no fabricated ordering coordinate and retain their native sequence.
    all_rows = records + list(snapshots.values())
    all_rows.sort(key=lambda r: r["output_sequence"])
    yield from all_rows


@click.command()
@click.option("--host", help="Receiver TCP hostname or unbracketed IPv6 address.")
@click.option("--port", type=click.IntRange(1, 65535), default=2006, show_default=True)
@click.option(
    "--input",
    "input_path",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    help="Replay UBX/SBF or XZ instead of TCP.",
)
@click.option("-p", "--protocol", type=click.Choice(["ubx", "sbf"]), required=True)
@click.option("--setup", "setup_path", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--max-latency", type=click.FloatRange(min=0, min_open=True), default=5.0, show_default=True)
@click.option("--duration", type=click.FloatRange(min=0, min_open=True), help="TCP capture seconds; default unlimited.")
@click.option(
    "--snapshot-interval",
    type=click.IntRange(1),
    default=3600,
    show_default=True,
    help="GPST-aligned snapshot interval in seconds.",
)
@click.option("--output", type=click.Path(path_type=Path), help="Exclusive-create JSONL file; default stdout.")
def cli(host, port, input_path, protocol, setup_path, max_latency, duration, snapshot_interval, output):
    """Decode GPS/QZSS LNAV CommonNEX messages and emit periodic snapshots."""
    if bool(host) == bool(input_path):
        raise click.UsageError("Specify exactly one of --host or --input")
    if input_path and duration is not None:
        raise click.UsageError("--duration applies only to TCP")
    try:
        setup = validate_setup(json.loads(setup_path.read_text()))
        seconds, fraction = setup["epoch_period_s"].split(".")
        stream = CnexStream(protocol, setup["setup_id"], int(seconds), int(fraction))
        consumer = BroadcastMessageDecoder(setup["setup_id"], snapshot_interval)
        groups = (
            file_groups(input_path, stream)
            if input_path
            else tcp_groups(host, port, stream, max_latency=max_latency, duration=duration)
        )
        click.echo("Decoding GPS/QZSS LNAV; other broadcast families are counted as unsupported.", err=True)
        with output.open("x") if output else nullcontext(click.get_text_stream("stdout")) as target:
            for group in groups:
                if group is None:
                    continue
                for row in json_records(consumer.feed(group)):
                    target.write(json.dumps(row, default=json_value, allow_nan=False) + "\n")
                target.flush()
                if group.notice:
                    click.echo(group.notice, err=True)
        click.echo(json.dumps(consumer.diagnostics, sort_keys=True), err=True)
    except (OSError, ValueError, RuntimeError, BufferError, EOFError, OverflowError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
