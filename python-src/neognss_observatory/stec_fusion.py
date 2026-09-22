# SPDX-License-Identifier: GPL-3.0-only
"""Equal-mean fusion of calibrated L1-anchored pairs, with explicit boundaries."""

import json
from datetime import timedelta
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from .stec_incremental import DAY_NS, EPOCH

SCHEMA = pa.schema(
    [
        ("gpst_ns", pa.int64()),
        ("satellite_system", pa.string()),
        ("prn", pa.int32()),
        ("fusion_group", pa.string()),
        ("segment_start_gpst_ns", pa.int64()),
        ("receiver_window_id", pa.int64()),
        ("receiver_segment_start_ns", pa.int64()),
        ("stec_absolute_tecu", pa.float64()),
        ("ipp_latitude_deg", pa.float64()),
        ("ipp_longitude_deg", pa.float64()),
        ("elevation_deg", pa.float64()),
        ("azimuth_deg", pa.float64()),
        ("pair_ids", pa.list_(pa.int32())),
        ("arc_ids", pa.list_(pa.int64())),
        ("window_ids", pa.list_(pa.int64())),
        ("weights", pa.list_(pa.float64())),
        ("pair_difference_tecu", pa.float64()),
        ("fusion_status", pa.string()),
        ("consistency_status", pa.string()),
        ("combination_changed", pa.bool_()),
        ("provisional", pa.bool_()),
    ]
)


def signature(row):
    return (row["receiver_segment_start_ns"], tuple(row["pair_ids"]), tuple(row["arc_ids"]), tuple(row["window_ids"]))


def fuse(table, pairs, previous, gap_ns):
    """Vectorized group reductions; row assembly only at the emitted cadence."""
    t = table["gpst_ns"].to_numpy()
    pid = table["pair_id"].to_numpy()
    prn = table["prn"].to_numpy()
    systems = np.array([p["system"] for p in pairs])[pid]
    groups = np.array([p["fusion_group"] for p in pairs])[pid]
    order = np.lexsort((pid, prn, groups, t))
    t = t[order]
    pid = pid[order]
    prn = prn[order]
    systems = systems[order]
    groups = groups[order]
    columns = {
        k: table[k].to_numpy()[order]
        for k in (
            "stec_absolute_tecu",
            "arc_id",
            "receiver_window_id",
            "receiver_segment_start_ns",
            "provisional",
            "ipp_latitude_deg",
            "ipp_longitude_deg",
            "elevation_deg",
            "azimuth_deg",
        )
    }
    edges = np.r_[0, np.flatnonzero((t[1:] != t[:-1]) | (groups[1:] != groups[:-1]) | (prn[1:] != prn[:-1])) + 1, len(t)]
    values = columns["stec_absolute_tecu"]
    valid = np.isfinite(values)
    counts = np.add.reduceat(valid.astype(int), edges[:-1])
    sums = np.add.reduceat(np.where(valid, values, 0), edges[:-1])
    # Convert once: avoid allocating tiny NumPy arrays for every satellite epoch.
    geometry = {}
    positions = np.arange(len(t))
    for k in ("ipp_latitude_deg", "ipp_longitude_deg", "elevation_deg", "azimuth_deg"):
        first = np.minimum.reduceat(np.where(np.isfinite(columns[k]), positions, len(t)), edges[:-1])
        geometry[k] = pa.array(columns[k][np.minimum(first, len(t) - 1)], mask=first == len(t))
    columns = {k: v.tolist() for k, v in columns.items() if k not in geometry}
    t, pid, prn, systems, groups = (v.tolist() for v in (t, pid, prn, systems, groups))
    valid = valid.tolist()
    values = values.tolist()
    out = {field.name: [] for field in SCHEMA if field.name not in geometry}
    states = {key: (signature(row), row["gpst_ns"], row["segment_start_gpst_ns"]) for key, row in previous.items()}
    family_ids = [p["family_id"] for p in pairs]
    bias_datums = [p["bias_datum"] for p in pairs]
    for i, (a, b) in enumerate(zip(edges[:-1], edges[1:])):
        ids = [j for j in range(a, b) if valid[j]]
        n = int(counts[i])
        status = "averaged" if n > 1 else "single_pair" if n else "unavailable"
        families = [family_ids[p] for p in pid[a:b]]
        if len(families) != len(set(families)):
            raise ValueError("Duplicate selected STEC family in epoch")
        if n > 2:
            raise ValueError("More than two allowed L1-anchored pairs in fusion group")
        if any(v != columns["receiver_segment_start_ns"][a] for v in columns["receiver_segment_start_ns"][a:b]):
            raise ValueError("Cannot fuse different receiver segments at one epoch")
        datums = {bias_datums[pid[j]] for j in ids}
        if len(datums) > 1:
            status = "incompatible_datum"
            ids = []
            n = 0
        row = dict(
            gpst_ns=int(t[a]),
            satellite_system=str(systems[a]),
            prn=int(prn[a]),
            fusion_group=str(groups[a]),
            receiver_segment_start_ns=int(columns["receiver_segment_start_ns"][a]),
            receiver_window_id=int(columns["receiver_window_id"][ids[0]]) if n else None,
            stec_absolute_tecu=float(sums[i] / n) if n else None,
            pair_ids=[pid[j] for j in ids],
            arc_ids=[columns["arc_id"][j] for j in ids],
            window_ids=[int(columns["receiver_window_id"][j]) for j in ids],
            weights=[1 / n] * n if n else [],
            pair_difference_tecu=float(values[ids[0]] - values[ids[1]]) if n == 2 else None,
            fusion_status=status,
            consistency_status="difference_reported_no_rejection_threshold" if n == 2 else "not_comparable",
            provisional=any(columns["provisional"][j] for j in ids) if n else True,
        )
        key = (row["fusion_group"], row["prn"])
        old = states.get(key)
        sig = signature(row)
        changed = old is None or sig != old[0] or row["gpst_ns"] - old[1] > gap_ns
        row["combination_changed"] = changed
        row["segment_start_gpst_ns"] = row["gpst_ns"] if changed else old[2]
        states[key] = sig, row["gpst_ns"], row["segment_start_gpst_ns"]
        previous[key] = row
        for name, value in row.items():
            out[name].append(value)
    return pa.Table.from_pydict(out | geometry, schema=SCHEMA)


def write_fused(root, metadata, from_ns=None):
    from .stec import read_stec

    destination = root / "fused"
    destination.mkdir(exist_ok=True)
    start = (from_ns // DAY_NS) * DAY_NS if from_ns is not None else None
    previous = {}
    paths = sorted(destination.glob("GPST-*.parquet"))
    earlier = [p for p in paths if start is not None and p.stem < "GPST-" + (EPOCH + timedelta(days=start // DAY_NS)).isoformat()]
    if earlier:
        for batch in pq.ParquetFile(earlier[-1]).iter_batches():
            for row in batch.to_pylist():
                previous[(row["fusion_group"], row["prn"])] = row
    pending = None
    day = None
    writer = None
    temporary = None
    path = None
    counts = {}

    def append(table):
        nonlocal day, writer, path, temporary
        for d in np.unique(table["gpst_ns"].to_numpy() // DAY_NS):
            rows = table.filter(table["gpst_ns"].to_numpy() // DAY_NS == d)
            if day != int(d):
                if writer:
                    writer.close()
                    temporary.replace(path)
                day = int(d)
                path = destination / f"GPST-{(EPOCH+timedelta(days=day)).isoformat()}.parquet"
                temporary = path.with_suffix(".parquet.new")
                schema = SCHEMA.with_metadata(
                    {b"ngo": json.dumps(metadata | dict(product="fused_stec", uncertainty="not inferred from pair count")).encode()}
                )
                writer = pq.ParquetWriter(temporary, schema, compression="zstd", compression_level=3)
            writer.write_table(rows.replace_schema_metadata(writer.schema.metadata))
            for value in rows["fusion_status"].to_pylist():
                counts[value] = counts.get(value, 0) + 1

    try:
        for table in read_stec(root, start):
            if pending is not None:
                table = pa.concat_tables([pending, table])
            last = table["gpst_ns"][-1].as_py()
            mask = pa.compute.less(table["gpst_ns"], last)
            complete = table.filter(mask)
            pending = table.filter(pa.compute.invert(mask))
            if len(complete):
                append(fuse(complete, metadata["pairs"], previous, round(metadata["settings"]["gap_timeout"] * 1e9)))
        if pending is not None and len(pending):
            append(fuse(pending, metadata["pairs"], previous, round(metadata["settings"]["gap_timeout"] * 1e9)))
    finally:
        if writer:
            writer.close()
    if temporary:
        temporary.replace(path)
    return counts


def read_fused(root, start_ns=None, end_ns=None):
    root = Path(root)
    for path in sorted((root / "fused").glob("*.parquet")):
        for batch in pq.ParquetFile(path).iter_batches():
            t = pa.Table.from_batches([batch])
            times = t["gpst_ns"].to_numpy()
            keep = np.ones(len(t), bool)
            if start_ns is not None:
                keep &= times >= start_ns
            if end_ns is not None:
                keep &= times < end_ns
            if np.any(keep):
                yield t.filter(keep)
