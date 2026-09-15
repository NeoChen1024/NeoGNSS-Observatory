# SPDX-License-Identifier: GPL-3.0-only
"""GPS phase leveling and incremental GIM-constrained DCB from CommonNEX."""

import json
import sys
import tempfile
import tomllib
import uuid
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .stec_incremental import (
    DAY_NS,
    EPOCH,
    DailySamples,
    observation_batches,
    publication,
    replace_json,
    replace_table,
    selection,
    signature,
    validate_events,
)
from .stec_products import StecProducts

K = 40.3e16 * (1227.60e6**-2 - 1575.42e6**-2)
NATIVE_DEFAULTS = dict(
    signal1="1C",
    signal2="2L",
    interval=30.0,
    gap_timeout=50.0,
    elevation_deg=10.0,
    level_elevation_deg=30.0,
    mapping_height_km=506.7,
    gf_jump_m=0.25,
    gf_rate_m_s=0.05,
    min_arc_seconds=600.0,
    min_level_samples=20,
)
DCB_DEFAULTS = dict(
    window_hours=24.0,
    bin_seconds=300.0,
    min_hours=6.0,
    min_satellites=4,
    min_arcs=6,
    elevation_deg=30.0,
    gim_rms_floor_tecu=2.0,
    max_scatter_tecu=15.0,
)


class BufferedTables:
    """Coalesce small native batches into useful compressed Parquet row groups."""

    def __init__(self, sink):
        self.sink, self.pending, self.count = sink, [], 0

    def append(self, rows):
        if len(rows):
            self.pending.append(rows)
            self.count += len(rows)
        if self.count >= 65536:
            self.flush()

    def flush(self):
        if self.pending:
            self.sink.append(np.concatenate(self.pending))
            self.pending.clear()
            self.count = 0

    def close(self):
        self.flush()
        self.sink.close()


def arc_levels(root):
    table = pq.read_table(root / "arcs.parquet", columns=["arc_id", "level_offset_m"])
    ids, offsets = (table[k].to_numpy() for k in table.column_names)
    order = np.argsort(ids)
    ids, offsets = ids[order], offsets[order]
    if len(ids) > 1 and np.any(np.diff(ids) <= 0):
        raise ValueError("Duplicate STEC arc ID")
    return ids, offsets


def leveled(table, ids, offsets):
    wanted = table["arc_id"].to_numpy()
    indices = np.searchsorted(ids, wanted)
    if np.any(indices >= len(ids)) or np.any(ids[indices] != wanted):
        raise ValueError("Missing finalized STEC arc")
    return (table["phase_gf_m"].to_numpy() + offsets[indices]) / K


def sample_batches(root, start_ns=None, end_ns=None):
    paths = sorted((root / "samples").glob("*.parquet"))
    if not paths:
        raise ValueError("No STEC sample tables")
    for path in paths:
        parquet = pq.ParquetFile(path)
        column = parquet.schema_arrow.get_field_index("gpst_ns")
        groups = []
        for i in range(parquet.metadata.num_row_groups):
            stats = parquet.metadata.row_group(i).column(column).statistics
            if stats is not None and stats.has_min_max:
                if start_ns is not None and stats.max < start_ns:
                    continue
                if end_ns is not None and stats.min >= end_ns:
                    continue
            groups.append(i)
        yield from parquet.iter_batches(batch_size=65536, row_groups=groups)


def calibrate(root, settings, metadata, from_ns=None, origin_ns=None, previous=None):
    """One bounded in-memory window; native robust fit, not per-row Python."""
    ids, offsets = arc_levels(root)
    window_ns = round(settings["window_hours"] * 3600e9)
    if window_ns < 1:
        raise ValueError("Invalid receiver-bias window")
    rows = [r for r in (previous or []) if from_ns is not None and r["end_gpst_ns"] <= from_ns]
    pieces, origin, current, last_time = [], origin_ns, None, None
    arc_table = pq.read_table(root / "arcs.parquet", columns=["gpst_ns", "end_ns", "provisional"])
    open_start = arc_table["gpst_ns"].to_numpy()[arc_table["provisional"].to_numpy().astype(bool)]
    open_end = arc_table["end_ns"].to_numpy()[arc_table["provisional"].to_numpy().astype(bool)]

    def flush(window_complete=False):
        if not pieces:
            return
        data = np.concatenate(pieces)
        fit = _native.fit_receiver_dcb(data, settings)
        fit.update(
            window_id=current,
            start_gpst_ns=origin + current * window_ns,
            end_gpst_ns=min(origin + (current + 1) * window_ns, last_time + 1),
            method="gim_constrained",
            signal_pair=f"C{metadata['signal1']}-C{metadata['signal2']}",
            provisional=bool(
                (not window_complete and last_time + 1 < origin + (current + 1) * window_ns)
                or np.any((open_start < origin + (current + 1) * window_ns) & (open_end >= origin + current * window_ns))
            ),
        )
        rows.append(fit)
        pieces.clear()

    for batch in tqdm(sample_batches(root, from_ns), desc="Receiver DCB", unit="batch"):
        table = pa.Table.from_batches([batch])
        if from_ns is not None:
            table = table.filter(pa.compute.greater_equal(table["gpst_ns"], from_ns))
        if not len(table):
            continue
        times = table["gpst_ns"].to_numpy()
        if origin is None:
            origin = int(times[0])
        if last_time is not None and times[0] < last_time:
            raise ValueError("Non-monotonic STEC tables")
        data = np.empty(len(table), dtype=_native.dcb_sample_dtype)
        for name in data.dtype.names:
            if name != "residual_tecu":
                data[name] = table[name].to_numpy()
        data["residual_tecu"] = leveled(table, ids, offsets) - table["gim_stec_tecu"].to_numpy()
        windows = (times - origin) // window_ns
        for window in np.unique(windows):
            if int(window) != current:
                flush(window_complete=True)
                current = int(window)
            selected = data[windows == window]
            pieces.append(selected)
            last_time = int(selected[-1]["gpst_ns"])
    flush()
    if not rows:
        raise ValueError("No receiver DCB windows")
    # Explicit nullable floats, including when every window is insufficient.
    floats = {"receiver_bias_tecu", "receiver_dcb_ns", "candidate_bias_tecu", "residual_scatter_tecu"}
    table = pa.table({key: pa.array([r[key] for r in rows], type=pa.float64() if key in floats else None) for key in rows[0]})
    table = table.replace_schema_metadata({b"ngo": json.dumps(metadata | dict(dcb=settings)).encode()})
    replace_table(root / "receiver_bias.parquet", table)
    return rows


def read_stec(root, start_ns=None, end_ns=None):
    """Yield finalized batches, without reopening raw observations or products.

    Invalid leveling or uncalibrated windows produce Arrow nulls, never a zero
    bias fallback. Negative estimates are retained, not silently clipped.
    """
    root = Path(root)
    ids, offsets = arc_levels(root)
    biases = pq.read_table(root / "receiver_bias.parquet")
    starts, ends, values = (biases[k].to_numpy() for k in ("start_gpst_ns", "end_gpst_ns", "receiver_bias_tecu"))
    window_ids = biases["window_id"].to_numpy()
    bias_provisional = biases["provisional"].to_numpy()
    arc_table = pq.read_table(root / "arcs.parquet", columns=["arc_id", "provisional"])
    order = np.argsort(arc_table["arc_id"].to_numpy())
    arc_provisional = arc_table["provisional"].to_numpy()[order].astype(bool)
    for batch in sample_batches(root, start_ns, end_ns):
        table = pa.Table.from_batches([batch])
        times = table["gpst_ns"].to_numpy()
        keep = np.ones(len(table), dtype=bool)
        if start_ns is not None:
            keep &= times >= start_ns
        if end_ns is not None:
            keep &= times < end_ns
        table = table.filter(keep)
        times = times[keep]
        if not len(table):
            continue
        indices = np.searchsorted(starts, times, side="right") - 1
        covered = (indices >= 0) & (times < ends[np.maximum(0, indices)])
        bias = np.where(covered, values[np.maximum(0, indices)], np.nan)
        table = table.append_column("receiver_window_id", pa.array(window_ids[np.maximum(0, indices)], mask=~covered))
        level = leveled(table, ids, offsets)
        arc_indices = np.searchsorted(ids, table["arc_id"].to_numpy())
        table = table.append_column(
            "provisional", pa.array(arc_provisional[arc_indices] | bias_provisional[np.maximum(0, indices)])
        )
        for name, data in (
            ("stec_leveled_tecu", level),
            ("receiver_bias_tecu", bias),
            ("stec_absolute_tecu", level - bias),
        ):
            table = table.append_column(name, pa.array(data, mask=~np.isfinite(data)))
        yield table


def antenna_position(setup):
    """Marker ECEF plus local north/east/up ARP offset; no hidden zero offset."""
    marker = setup.get("marker") or {}
    antenna = setup["antenna"]
    xyz, neu = marker.get("position_xyz_m"), antenna.get("arp_offset_neu_m")
    if xyz is None or neu is None:
        raise ValueError("Provide Setup marker XYZ and ARP NEU, or explicit position_ecef_m in STEC config")
    xyz = np.array([xyz[k] for k in ("x", "y", "z")], dtype=float)
    lon = np.arctan2(xyz[1], xyz[0])
    lat = np.arctan2(xyz[2], np.hypot(*xyz[:2]) * (1 - 6.6943799901413165e-3))
    for _ in range(10):
        radius = 6378137 / np.sqrt(1 - 6.6943799901413165e-3 * np.sin(lat) ** 2)
        lat = np.arctan2(xyz[2] + 6.6943799901413165e-3 * radius * np.sin(lat), np.hypot(*xyz[:2]))
    north = np.array([-np.sin(lat) * np.cos(lon), -np.sin(lat) * np.sin(lon), np.cos(lat)])
    east = np.array([-np.sin(lon), np.cos(lon), 0])
    up = np.array([np.cos(lat) * np.cos(lon), np.cos(lat) * np.sin(lon), np.sin(lat)])
    return (xyz + neu["north"] * north + neu["east"] * east + neu["up"] * up).tolist()


@click.command()
@click.option(
    "--input-dir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    required=True,
    help="Initialized CommonNEX station with daily Observation Parquet.",
)
@click.option("--config", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--start", help="Inclusive GPST date YYYY-MM-DD; keep unchanged on continuation.")
@click.option("--end", help="Exclusive GPST date YYYY-MM-DD; may extend on later invocations.")
@click.option("--rebuild", is_flag=True, help="Recompute the complete selection; preserve previous output as backup.")
def cli(input_dir, config, output, start, end, rebuild):
    """Incrementally process CommonNEX GPS observations; no raw protocol input."""
    from datetime import date

    try:
        if start is not None:
            start = date.fromisoformat(start).isoformat()
        if end is not None:
            end = date.fromisoformat(end).isoformat()
        if start and end and end <= start:
            raise ValueError("End must follow start")
        input_dir = input_dir.resolve()
        setup, days = selection(input_dir, start, end)
        settings = tomllib.loads(config.read_text())
        required = {"products_root"}
        if required - settings.keys():
            raise ValueError(f"Missing STEC settings: {sorted(required-settings.keys())}")
        allowed = required | NATIVE_DEFAULTS.keys() | {"station", "position_ecef_m", "dcb", "margin_hours"}
        if settings.keys() - allowed:
            raise ValueError(f"Unknown STEC settings: {sorted(settings.keys()-allowed)}")
        native = NATIVE_DEFAULTS | {k: v for k, v in settings.items() if k in NATIVE_DEFAULTS}
        native["position_ecef_m"] = settings.get("position_ecef_m")
        if native["position_ecef_m"] is None:
            native["position_ecef_m"] = antenna_position(setup)
        dcb = DCB_DEFAULTS | settings.get("dcb", {})
        if dcb.keys() - DCB_DEFAULTS.keys() or not all(np.isfinite(v) and v > 0 for v in dcb.values()):
            raise ValueError("Invalid receiver DCB settings")
        if dcb["window_hours"] < dcb["min_hours"] or dcb["bin_seconds"] < native["interval"]:
            raise ValueError("DCB window must cover min_hours; bin_seconds must cover sample interval")
        products_root = Path(settings["products_root"]).expanduser()
        if not products_root.is_absolute():
            products_root = config.resolve().parent / products_root
        products_root = products_root.resolve()
        if not products_root.is_dir():
            raise ValueError("Missing local CDDIS product root")
        for source in (input_dir, products_root, config.resolve()):
            if output.resolve().is_relative_to(source) or source.is_relative_to(output.resolve()):
                raise ValueError("STEC output must not contain or be inside an input")
        contract = dict(
            input_dir=str(input_dir),
            setup=setup,
            native=native,
            dcb=dcb,
            products_root=str(products_root),
            margin_hours=settings.get("margin_hours", 6),
            station=settings.get("station", setup["setup_id"]),
            start=start,
        )
        old = None
        if output.exists() and not rebuild:
            state_path = output / "state.json"
            if not state_path.is_file():
                raise ValueError("Existing output has no CommonNEX STEC state; use --rebuild")
            old = json.loads(state_path.read_text())
            if old.get("version") != 1 or not old.get("lineage") or old["contract"] != contract:
                raise ValueError("STEC configuration/Setup changed; use --rebuild with the complete selection")
        selected = {str(p.relative_to(input_dir)): signature(p, input_dir) for _, obs, ev in days for p in [*obs, *ev]}
        if old:
            for name, entry in old["inputs"].items():
                if selected.get(name) != entry:
                    raise ValueError(f"Previously consumed CommonNEX part changed/revised/removed: {name}; use --rebuild")
            if old["inputs"] == selected:
                click.echo(json.dumps(dict(status="up-to-date")))
                return
        consumed = old["inputs"] if old else {}
        todo = [
            (
                label,
                [p for p in obs if str(p.relative_to(input_dir)) not in consumed],
                [p for p in ev if str(p.relative_to(input_dir)) not in consumed],
            )
            for label, obs, ev in days
        ]
        for _, _, events in todo:
            validate_events(events, setup["setup_id"])
        processor = _native.StecProcessor(native)
        if old:
            processor.restore(old["processor"])
        metadata = dict(
            schema_version=2,
            time_scale="GPST",
            time_epoch="1980-01-06",
            station=settings.get("station", setup["setup_id"]),
            constellation="GPS",
            signal1=native["signal1"],
            signal2=native["signal2"],
            meters_per_tecu=K,
            arc_reasons=processor.summary()["arc_reasons"],
            product_issues={
                "1": "orbit unavailable",
                "2": "clock unavailable",
                "4": "health unavailable",
                "8": "satellite bias unavailable",
                "16": "GIM unavailable",
            },
            missing_product_policy="preserve phase continuity; unavailable dependent fields; no broadcast or zero-bias fallback",
            missing_values="NaN for unavailable numerical samples/arc fields; null for unestimated receiver bias",
            settings=native,
            dcb=dcb,
            units="gpst_ns/end_ns: nanoseconds; *_m: meters; *_deg: degrees; *_tecu: TECU; mapping: dimensionless",
            absolute_reference="GIM-constrained estimate, not independently calibrated TEC",
            receiver_bias_sign="P2-P1 additive receiver delay; absolute=leveled-receiver_bias",
            satellite_bias="IONEX C1W-C2W datum + MGEX within-frequency exact-signal OSB differences",
            gim="COD0OPSFIN; external UT normalized to GPST; sun-fixed interpolation",
            ipp="geocentric intersection at 6821 km; CODE MSLM alpha=0.9782",
            limitations=[
                "GPS L1/L2 only",
                "no phase wind-up or antenna phase-centre correction",
                "code multipath and receiver code smoothing can bias leveling",
                "GIM model error can alias into receiver DCB; scatter is not absolute uncertainty",
                "constant effective receiver bias per estimation window; no temperature model",
            ],
        )

        metadata["input_format"] = "CommonNEX"
        metadata["timestamp_conversion"] = "decimal GPST to integer ns, round-half-to-even; collisions rejected"
        with publication(output, rebuild) as stage:
            existing_arcs = pq.read_table(stage / "arcs.parquet") if old else None
            closed = [existing_arcs.filter(pa.compute.equal(existing_arcs["provisional"], 0))] if old else []
            # Only windows touched by new samples or previously open arcs need refitting.
            dirty = min((t["start"] for t in old["processor"]["tracks"]), default=None) if old else None
            samples = BufferedTables(DailySamples(stage, metadata))
            reader = _native.StecCnexReader(setup["setup_id"], native["signal1"], native["signal2"])
            new_sample_start = None
            source_bytes = sum(p.stat().st_size for _, obs, _ in todo for p in obs)
            missing = set(old.get("missing_products", [])) if old else set()
            try:
                with tempfile.TemporaryDirectory(prefix="ngo-stec-products-") as scratch:
                    products = StecProducts(
                        products_root, scratch, native["signal1"], native["signal2"], settings.get("margin_hours", 6)
                    )
                    with tqdm(total=source_bytes, desc="CommonNEX STEC", unit="B", unit_scale=True, mininterval=1) as progress:

                        def process(batch):
                            nonlocal new_sample_start
                            if not batch.size:
                                return
                            selected_products = products.prepare(batch.start_ns, batch.end_ns)
                            if selected_products is not None:
                                for name in sorted(products.missing - missing):
                                    progress.write(
                                        f"Warning: missing product {name}; affected fields unavailable.", file=sys.stderr
                                    )
                                missing.update(products.missing)
                                processor.products(selected_products)
                            points, levels = processor.process(batch)
                            if len(points):
                                new_sample_start = int(points["gpst_ns"][0]) if new_sample_start is None else new_sample_start
                                samples.append(points)
                            if len(levels):
                                closed.append(pa.table({n: levels[n] for n in levels.dtype.names}))

                        for _, paths, _ in todo:
                            for path in paths:
                                for batch in observation_batches([path], setup["setup_id"]):
                                    process(reader.feed(batch))
                                progress.update(path.stat().st_size)
                            # CommonNEX publishes complete measurement epochs; physical parts may split rows.
                            process(reader.flush())
            finally:
                samples.close()
            checkpoint = processor.checkpoint()
            summary = processor.summary()
            reader_stats = reader.summary()
            for key in reader_stats:
                reader_stats[key] += old.get("reader", {}).get(key, 0) if old else 0
            if not summary["samples"]:
                raise ValueError("No usable selected GPS dual-frequency STEC samples")
            preview = processor.preview()
            closed.append(pa.table({n: preview[n] for n in preview.dtype.names}))
            arcs = pa.concat_tables([t.replace_schema_metadata(None) for t in closed]).sort_by("arc_id")
            arcs = arcs.replace_schema_metadata({b"ngo": json.dumps(metadata).encode()})
            replace_table(stage / "arcs.parquet", arcs)
            origin = old["origin_ns"] if old else new_sample_start
            candidates = [t for t in (dirty, new_sample_start) if t is not None]
            from_ns = None
            if old and candidates:
                width = round(dcb["window_hours"] * 3600e9)
                from_ns = origin + max(0, (min(candidates) - origin) // width) * width
            previous_fits = pq.read_table(stage / "receiver_bias.parquet").to_pylist() if old else None
            fits = calibrate(stage, dcb, metadata, from_ns, origin, previous_fits) if candidates or not old else previous_fits
            generation = old["generation"] + 1 if old else 1
            lineage = old["lineage"] if old else uuid.uuid4().hex
            changed_from = from_ns if old else origin
            day_generations = old.get("day_generations", {}).copy() if old else {}
            for path in sorted((stage / "samples").glob("*.parquet")):
                label = path.stem.removeprefix("GPST-")
                if not old or (candidates and (date.fromisoformat(label) - EPOCH).days * DAY_NS + DAY_NS > changed_from):
                    day_generations[label] = generation
            estimated = sum(r["status"] == "estimated" for r in fits)
            summary.update(
                status=(
                    "partial_calibration"
                    if estimated != len(fits)
                    else "partial_products" if any(summary["product_gaps"].values()) else "complete"
                ),
                calibrated_windows=estimated,
                windows=len(fits),
                metadata=metadata,
                missing_product_files=sorted(missing),
                reader=reader_stats,
                generation=generation,
                lineage=lineage,
                day_generations=day_generations,
                provisional_arcs=len(preview),
                input_format="CommonNEX",
            )
            replace_json(stage / "summary.json", summary)
            replace_json(
                stage / "state.json",
                dict(
                    version=1,
                    contract=contract,
                    inputs=selected,
                    processor=checkpoint,
                    reader=reader_stats,
                    origin_ns=origin,
                    generation=generation,
                    lineage=lineage,
                    day_generations=day_generations,
                    missing_products=sorted(missing),
                ),
            )
            if any(signature(input_dir / name, input_dir) != entry for name, entry in selected.items()):
                raise ValueError("CommonNEX inputs changed during processing; published output was not replaced")
        click.echo(
            json.dumps({k: summary[k] for k in ("status", "samples", "arcs", "provisional_arcs", "calibrated_windows", "windows")})
        )
    except (ValueError, RuntimeError, OSError, KeyError, TypeError, pa.ArrowException) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
