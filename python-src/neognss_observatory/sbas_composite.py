# SPDX-License-Identifier: GPL-3.0-only
"""Select SBAS sources on interval boundaries, then form hourly means."""

import itertools

import click
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from .sbas_grid_parquet import SCHEMA

PRIORITY = ("MSAS", "BDSBAS", "KASS", "GAGAN", "SOUTHPAN")
# RINEX Sxx numbers, not receiver-specific SVIDs or PRN minus 87.
PROVIDERS = {
    29: "MSAS",
    37: "MSAS",
    30: "BDSBAS",
    43: "BDSBAS",
    44: "BDSBAS",
    34: "KASS",
    42: "KASS",
    27: "GAGAN",
    28: "GAGAN",
    32: "GAGAN",
    22: "SOUTHPAN",
}
SOURCE = pa.struct(
    [("provider", pa.string()), ("satellite_number", pa.int64()), ("valid_seconds", pa.float64()), ("mt0_seconds", pa.float64())]
)
META = {
    b"time_scale": b"GPST",
    b"time_origin": b"1980-01-06 00:00:00 GPST",
    b"quantity": b"SBAS composite VTEC",
    b"selection": b"provider priority before time-weighted averaging",
    b"mt0_policy": SCHEMA.metadata[b"mt0_policy"],
}
HOURLY_SCHEMA = pa.schema(
    [
        ("hour_gpst", pa.int64()),
        ("latitude", pa.float64()),
        ("longitude", pa.float64()),
        ("vtec_integral_tecu_seconds", pa.float64()),
        ("valid_seconds", pa.float64()),
        ("coverage", pa.float64()),
        ("vtec_tecu", pa.float64()),
        ("source_changes", pa.int64()),
        ("sources", pa.list_(SOURCE)),
    ],
    metadata=META,
)
SELECTED_SCHEMA = pa.schema(
    [
        ("start_gpst_ms", pa.int64()),
        ("end_gpst_ms", pa.int64()),
        ("latitude", pa.float64()),
        ("longitude", pa.float64()),
        ("provider", pa.string()),
        ("satellite_number", pa.int64()),
        ("vtec_tecu", pa.float64()),
        ("reported_gpst_ms", pa.int64()),
        ("mt0_seen", pa.bool_()),
        ("selection_reason", pa.string()),
    ],
    metadata=META,
)


def parse_priority(ctx, param, value):
    names = tuple(x.strip().upper() for x in value.split(","))
    if len(names) != len(PRIORITY) or set(names) != set(PRIORITY):
        raise click.BadParameter("List MSAS,BDSBAS,KASS,GAGAN,SouthPAN exactly once, in preferred order")
    return names


def composite_day(path, day, start=None, end=None, priority=PRIORITY, state=None):
    """One day of independent source intervals; state retains latest report evidence."""
    if day % 86400000:
        raise ValueError("Expected GPST midnight")
    with pq.ParquetFile(path) as source:
        schema = source.schema_arrow
        if not schema.equals(SCHEMA) or any(schema.metadata.get(k) != v for k, v in SCHEMA.metadata.items()):
            raise ValueError(f"Expected SBAS grid schema 4; regenerate ngo-sbas-grid-parquet: {path}")
        table = source.read()
    if not len(table):
        return [], []
    a = {name: table[name].to_numpy(zero_copy_only=False) for name in table.column_names}
    begin, finish, reported = (a[k] for k in ("start_gpst_ms", "end_gpst_ms", "reported_gpst_ms"))
    usable = a["status"] == "usable"
    if (
        np.any(begin < day)
        or np.any(finish > day + 86400000)
        or np.any(begin >= finish)
        or np.any(reported > begin)
        or np.any(reported < 0)
        or np.any(~np.isin(a["status"], ["usable", "do_not_use", "not_monitored"]))
        or np.any(usable & (~np.isfinite(a["vtec_tecu"]) | (a["vtec_tecu"] < 0)))
        or not np.isfinite(a["latitude"]).all()
        or not np.isfinite(a["longitude"]).all()
        or np.any(a["satellite_system"] != "S")
        or np.any(a["signal"] != "L1CA")
    ):
        raise ValueError(f"Invalid SBAS intervals: {path}")
    # Input row order is not global time order; validate each original grid stream.
    order = np.lexsort((begin, a["mask_bit"], a["band"], a["satellite_number"]))
    left, right = order[:-1], order[1:]
    same = (
        (a["satellite_number"][left] == a["satellite_number"][right])
        & (a["band"][left] == a["band"][right])
        & (a["mask_bit"][left] == a["mask_bit"][right])
    )
    if np.any(same & (finish[left] > begin[right])):
        raise ValueError("Overlapping SBAS source intervals")
    unknown = sorted(set(a["satellite_number"]) - PROVIDERS.keys())
    if unknown:
        raise ValueError(f"No provider mapping for SBAS sources {unknown}; define their identity before compositing")
    providers = np.array([PROVIDERS[int(s)] for s in a["satellite_number"]])
    state = {} if state is None else state
    stats, selected = {}, []
    longitude = (a["longitude"] + 180) % 360 - 180
    order = np.lexsort((longitude, a["latitude"]))
    splits = np.flatnonzero((np.diff(longitude[order]) != 0) | (np.diff(a["latitude"][order]) != 0)) + 1
    for indices in np.split(order, splits):
        lat, lon = float(a["latitude"][indices[0]]), float(longitude[indices[0]])
        latest = state.setdefault((lat, lon), {})
        events = sorted([(int(begin[i]), 1, int(i)) for i in indices] + [(int(finish[i]), 0, int(i)) for i in indices])
        active, previous_source = set(), None
        groups = iter(itertools.groupby(events, key=lambda e: e[0]))
        current = next(groups, None)
        while current is not None:
            t, changes = current
            for _, opening, i in changes:
                if opening:
                    active.add(i)
                    p = providers[i]
                    old_time, rejected = latest.get(p, (-1, False))
                    if reported[i] > old_time:
                        latest[p] = (int(reported[i]), not usable[i])
                    elif reported[i] == old_time:
                        latest[p] = (old_time, rejected or not usable[i])
                else:
                    active.remove(i)
            following = next(groups, None)
            if following is None:
                break
            stop = following[0]
            chosen = None
            for provider in priority:
                report_time, rejected = latest.get(provider, (-1, False))
                if rejected:
                    continue
                candidates = [i for i in active if providers[i] == provider and reported[i] == report_time]
                if not candidates or any(not usable[i] for i in candidates):
                    continue
                chosen = min(candidates, key=lambda i: (a["satellite_number"][i], a["band"][i], a["mask_bit"][i]))
                break
            if chosen is None:
                previous_source = None
            else:
                i = chosen
                identity = (str(providers[i]), int(a["satellite_number"][i]))
                changed = previous_source is not None and previous_source != identity
                previous_source = identity
                lo, hi = max(t, start * 1000 if start is not None else day), min(
                    stop, end * 1000 if end is not None else day + 86400000
                )
                if lo < hi:
                    record = dict(
                        start_gpst_ms=lo,
                        end_gpst_ms=hi,
                        latitude=lat,
                        longitude=lon,
                        provider=identity[0],
                        satellite_number=identity[1],
                        vtec_tecu=float(a["vtec_tecu"][i]),
                        reported_gpst_ms=int(reported[i]),
                        mt0_seen=bool(a["mt0_seen"][i]),
                        selection_reason="preferred" if identity[0] == priority[0] else "fallback",
                    )
                    if (
                        selected
                        and all(selected[-1][k] == v for k, v in record.items() if k not in ("start_gpst_ms", "end_gpst_ms"))
                        and selected[-1]["end_gpst_ms"] == lo
                    ):
                        selected[-1]["end_gpst_ms"] = hi
                    else:
                        selected.append(record)
                while lo < hi:
                    hour = lo // 3600000 * 3600
                    edge = min(hi, (hour + 3600) * 1000)
                    seconds = (edge - lo) / 1000
                    stat = stats.setdefault((hour, lat, lon), dict(integral=0.0, seconds=0.0, changes=0, sources={}))
                    stat["integral"] += a["vtec_tecu"][i] * seconds
                    stat["seconds"] += seconds
                    stat["changes"] += int(changed and lo == t)
                    source = stat["sources"].setdefault(identity, [0.0, 0.0])
                    source[0] += seconds
                    source[1] += seconds if a["mt0_seen"][i] else 0
                    lo = edge
            current = following
    rows = []
    for (hour, lat, lon), stat in sorted(stats.items()):
        seconds = stat["seconds"]
        if not 0 < seconds <= 3600.000001:
            raise ValueError("Composite coverage exceeds one hour")
        rows.append(
            dict(
                hour_gpst=hour,
                latitude=lat,
                longitude=lon,
                vtec_integral_tecu_seconds=float(stat["integral"]),
                valid_seconds=seconds,
                coverage=seconds / 3600,
                vtec_tecu=float(stat["integral"]) / seconds,
                source_changes=stat["changes"],
                sources=[
                    dict(provider=p, satellite_number=s, valid_seconds=v[0], mt0_seconds=v[1])
                    for (p, s), v in sorted(stat["sources"].items())
                ],
            )
        )
    selected.sort(key=lambda r: (r["start_gpst_ms"], r["latitude"], r["longitude"]))
    return rows, selected


def hourly_rows(path, day, start=None, end=None, priority=PRIORITY, state=None):
    return composite_day(path, day, start, end, priority, state)[0]
