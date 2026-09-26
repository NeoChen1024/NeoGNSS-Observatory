# SPDX-License-Identifier: GPL-3.0-only
"""Presentation-only selection among per-source SBAS snapshots."""

from collections import defaultdict

import click
import pyarrow.parquet as pq

PRIORITY = ("MSAS", "BDSBAS", "KASS", "GAGAN", "SOUTHPAN")
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


def parse_priority(ctx, param, value):
    names = tuple(x.strip().upper() for x in value.split(","))
    if len(names) != len(PRIORITY) or set(names) != set(PRIORITY):
        raise click.BadParameter("List MSAS,BDSBAS,KASS,GAGAN,SouthPAN exactly once, in preferred order")
    return names


def grid_files(root):
    return sorted(root.rglob("grid.parquet"))


def iter_snapshots(path):
    """Read one snapshot at a time without materializing an entire day."""
    stamp, rows = None, []
    with pq.ParquetFile(path) as source:
        for batch in source.iter_batches(batch_size=16384):
            for row in batch.to_pylist():
                t = row["snapshot_gpst"]
                if stamp is not None and t < stamp:
                    raise ValueError("SBAS snapshot time moved backwards")
                if stamp is not None and t != stamp:
                    yield stamp, rows
                    rows = []
                stamp = t
                rows.append(row)
    if rows:
        yield stamp, rows


def select_snapshot(rows, priority=PRIORITY, quantity="current", min_coverage=0):
    """Choose existing snapshot values; never reconstruct an interval composite."""
    cells = defaultdict(list)
    for row in rows:
        cells[row["latitude"], (row["longitude"] + 180) % 360 - 180].append(row)
    result = []
    for (lat, lon), candidates in sorted(cells.items()):
        providers = defaultdict(list)
        for row in candidates:
            providers[PROVIDERS.get(row["satellite_number"], f"UNKNOWN_S{row['satellite_number']:02}")].append(row)
        for provider in (*priority, *sorted(set(providers) - set(priority))):
            options = providers.get(provider, [])
            if quantity == "current" and options:
                latest = max(r["reported_gpst"] for r in options if r["reported_gpst"] is not None)
                options = [r for r in options if r["reported_gpst"] == latest]
                # Do not hide a same-provider alarm behind an older valid GEO.
                if any(r["status"] in ("DO_NOT_USE", "NOT_MONITORED") for r in options):
                    continue
                options = [r for r in options if r["status"] == "USABLE" and r["vtec_tecu"] is not None]
            else:
                options = [r for r in options if r["mean_vtec_tecu"] is not None]
            options = [r for r in options if r["coverage"] >= min_coverage]
            if not options:
                continue
            chosen = min(options, key=lambda r: (r["satellite_number"], r["band"], r["mask_bit"]))
            result.append(
                dict(
                    snapshot_gpst=int(chosen["snapshot_gpst"]),
                    latitude=lat,
                    longitude=lon,
                    vtec_tecu=chosen["vtec_tecu"] if quantity == "current" else chosen["mean_vtec_tecu"],
                    coverage=chosen["coverage"],
                    quantity=quantity,
                    sources=[
                        dict(
                            provider=provider,
                            satellite_number=chosen["satellite_number"],
                            mt0_restricted=(
                                chosen["mt0_restriction"] in ("ACTIVE", "UNKNOWN")
                                if quantity == "current"
                                else chosen["mt0_duration_s"] > 0
                            ),
                        )
                    ],
                )
            )
            break
    return result
