#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Typed canonical broadcast decoding; JSONL is only the example CLI output."""

import json
from decimal import Decimal

import click
import pyarrow as pa
import pyarrow.compute as pc

from . import _native
from .cnex_stream import (
    LIVE_ERRORS,
    live_options,
    live_source,
    restart_pieces,
    untimed_restarts,
    write_jsonl,
)


def aggregate_almanacs(output):
    """Merge equal snapshot candidates only; MessageOutput stays source-local."""
    for source, entries, summary in (
        ("snapshot_almanac_entry", "snapshot_almanac_entries", "snapshot_almanac_sets"),
        ("snapshot_cnav_reduced_almanac", "snapshot_cnav_reduced_almanac_entries", "snapshot_cnav_reduced_almanac_sets"),
        ("snapshot_cnav_midi_almanac", "snapshot_cnav_midi_almanac_entries", "snapshot_cnav_midi_almanac_sets"),
        ("snapshot_gal_inav_almanac", "snapshot_gal_inav_almanac_entries", "snapshot_gal_inav_almanac_sets"),
        ("snapshot_gal_fnav_almanac", "snapshot_gal_fnav_almanac_entries", "snapshot_gal_fnav_almanac_sets"),
    ):
        _aggregate_almanacs(output, source, entries, summary)
    for family in ("d1", "d2", "bcnav1", "bcnav2", "bcnav3"):
        for kind in ("almanac",) if family in ("d1", "d2") else ("midi_almanac", "reduced_almanac"):
            source = f"snapshot_bds_{family}_{kind}"
            _aggregate_almanacs(output, source, source + "_entries", source + "_sets")
    return output


def _aggregate_almanacs(output, source_kind, entries_kind, summary_kind):
    batches = output.pop(source_kind, [])
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
        "message_type",
        "prn_id_raw",
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
    output[entries_kind] = [pa.RecordBatch.from_pylist(list(candidates.values()), schema=schema)]
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
    output[summary_kind] = [
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
        self.restarts = []

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
            resets.extend(t for t in selected.column("gpst").to_pylist() if t is not None)
        resets.sort()
        # Untimed restarts are placed by arrival coordinate; one later than every
        # row seen so far waits for following RawBits.
        self.restarts.extend(untimed_restarts(group.catalogs.get("events", ())))
        result = {}

        def consume(batch):
            for kind, values in self.process(batch).items():
                result.setdefault(kind, []).extend(values)

        pieces = (
            piece
            for whole in group.catalogs.get("raw-bits", ())
            for piece in restart_pieces(whole, self.restarts, self.native.discontinuity)
        )
        for batch in pieces:
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
        if group.discontinuity:
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
@live_options
@click.option(
    "--snapshot-interval",
    type=click.IntRange(1),
    default=3600,
    show_default=True,
    help="GPST-aligned snapshot interval in seconds.",
)
def cli(host, port, input_path, protocol, setup_path, max_latency, duration, output, snapshot_interval):
    """Decode GPS/QZSS, Galileo, BeiDou and SBAS L1 from CommonNEX."""
    try:
        setup, groups = live_source(host, port, input_path, protocol, setup_path, max_latency, duration)
        consumer = BroadcastMessageDecoder(setup["setup_id"], snapshot_interval)
        click.echo("Decoding GPS/QZSS, Galileo, BeiDou navigation/corrections and SBAS L1.", err=True)
        write_jsonl(groups, lambda group: json_records(consumer.feed(group)), output, json_value)
        click.echo(json.dumps(consumer.diagnostics, sort_keys=True), err=True)
    except LIVE_ERRORS as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
