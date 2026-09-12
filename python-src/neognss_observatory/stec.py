# SPDX-License-Identifier: GPL-3.0-only
"""GPS phase leveling and GIM-constrained receiver DCB from raw UBX/SBF."""

import json
import sys
import tempfile
import tomllib
from pathlib import Path

import click
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from tqdm import tqdm

from . import _native
from .dataset_inputs import recordings
from .ppp import DailyTables, time_ns
from .protocol import ProtocolWarnings, protocol_option
from .research_output import staged_output, write_json
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


class ArcTable:
    def __init__(self, path, metadata):
        self.path, self.metadata, self.writer = path, metadata, None

    def append(self, rows):
        if not len(rows):
            return
        table = pa.table({name: rows[name] for name in rows.dtype.names})
        table = table.replace_schema_metadata({b"ngo": json.dumps(self.metadata).encode()})
        if self.writer is None:
            self.writer = pq.ParquetWriter(self.path, table.schema, compression="zstd", compression_level=3)
        self.writer.write_table(table)

    def close(self):
        if self.writer:
            self.writer.close()


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


def calibrate(root, settings, metadata):
    """One bounded in-memory window; native robust fit, not per-row Python."""
    ids, offsets = arc_levels(root)
    window_ns = round(settings["window_hours"] * 3600e9)
    if window_ns < 1:
        raise ValueError("Invalid receiver-bias window")
    rows, pieces, origin, current, last_time = [], [], None, 0, None

    def flush():
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
        )
        rows.append(fit)
        pieces.clear()

    for batch in tqdm(sample_batches(root), desc="Receiver DCB", unit="batch"):
        table = pa.Table.from_batches([batch])
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
                flush()
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
    pq.write_table(table, root / "receiver_bias.parquet", compression="zstd", compression_level=3)
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
        for name, data in (
            ("stec_leveled_tecu", level),
            ("receiver_bias_tecu", bias),
            ("stec_absolute_tecu", level - bias),
        ):
            table = table.append_column(name, pa.array(data, mask=~np.isfinite(data)))
        yield table


@click.command()
@protocol_option
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--config", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--start", help="Inclusive GPST calendar time.")
@click.option("--end", help="Exclusive GPST calendar time.")
@click.option("--overwrite", is_flag=True)
@staged_output
def cli(protocol, input_dir, config, output, start, end):
    """Extract GPS phase-leveled STEC and estimate GIM-constrained receiver DCB."""
    try:
        settings = tomllib.loads(config.read_text())
        required = {"station", "position_ecef_m", "products_root"}
        if required - settings.keys():
            raise ValueError(f"Missing STEC settings: {sorted(required-settings.keys())}")
        allowed = required | NATIVE_DEFAULTS.keys() | {"dcb", "margin_hours"}
        if settings.keys() - allowed:
            raise ValueError(f"Unknown STEC settings: {sorted(settings.keys()-allowed)}")
        native = NATIVE_DEFAULTS | {k: v for k, v in settings.items() if k in NATIVE_DEFAULTS}
        native["position_ecef_m"] = settings["position_ecef_m"]
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
        if output.resolve().is_relative_to(products_root) or products_root.is_relative_to(output.resolve()):
            raise ValueError("STEC output and product input must not contain each other")
        paths = recordings(input_dir.resolve(), protocol, True)
        start_ns, end_ns = time_ns(start), time_ns(end)
        start_ns = 0 if start_ns is None else start_ns
        end_ns = 2**63 - 1 if end_ns is None else end_ns
        if end_ns <= start_ns:
            raise ValueError("End must follow start")
        reader = _native.ObservationReader(protocol)
        processor = _native.StecProcessor(native)
        metadata = dict(
            schema_version=1,
            time_scale="GPST",
            time_epoch="1980-01-06",
            station=settings["station"],
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
        output.mkdir()
        samples = BufferedTables(DailyTables(output, "samples", metadata))
        arcs = BufferedTables(ArcTable(output / "arcs.parquet", metadata))
        try:
            with tempfile.TemporaryDirectory(prefix="ngo-stec-products-") as scratch:
                products = StecProducts(
                    products_root, scratch, native["signal1"], native["signal2"], settings.get("margin_hours", 6)
                )
                done = False
                warned_files, warned_issues = set(), 0
                with tqdm(
                    total=sum(p.stat().st_size for p in paths), desc="GPS STEC", unit="B", unit_scale=True, mininterval=1
                ) as progress:
                    for path in paths:
                        warnings = ProtocolWarnings(path, protocol, reader.summary())
                        with path.open("rb") as stream:
                            while data := stream.read(1024 * 1024):
                                batch = reader.feed(data)
                                progress.update(len(data))
                                warnings.update(reader.summary())
                                reached_end = batch.size and batch.end_ns >= end_ns
                                batch = batch.window(start_ns, end_ns)
                                if batch.size:
                                    selected = products.prepare(batch.start_ns, batch.end_ns)
                                    if selected:
                                        for name in sorted(products.missing - warned_files):
                                            progress.write(
                                                f"Warning: missing product {name}; affected fields will be unavailable.",
                                                file=sys.stderr,
                                            )
                                        warned_files.update(products.missing)
                                        processor.products(selected)
                                    points, levels = processor.process(batch)
                                    if len(points):
                                        issues = int(np.bitwise_or.reduce(points["product_issues"]))
                                        for bit, description in metadata["product_issues"].items():
                                            if issues & int(bit) and not warned_issues & int(bit):
                                                progress.write(
                                                    f"Warning: {description} for some observations; preserving phase and continuing.",
                                                    file=sys.stderr,
                                                )
                                        warned_issues |= issues
                                    samples.append(points)
                                    arcs.append(levels)
                                if reached_end:
                                    done = True
                                    break
                        warnings.update(reader.summary(), final=True)
                        if done:
                            break
                if not done:
                    reader.finish()
                arcs.append(processor.finish())
        finally:
            samples.close()
            arcs.close()
        summary = processor.summary()
        if not summary["samples"]:
            raise ValueError("No usable GPS STEC samples")
        fits = calibrate(output, dcb, metadata)
        estimated = sum(r["status"] == "estimated" for r in fits)
        summary.update(
            status=(
                ("partial_products" if any(summary["product_gaps"].values()) else "complete")
                if estimated == len(fits)
                else "partial_calibration"
            ),
            missing_product_files=sorted(products.missing),
            reader=reader.summary(),
            calibrated_windows=estimated,
            windows=len(fits),
            metadata=metadata,
        )
        write_json(output / "summary.json", summary)
        if estimated != len(fits):
            click.echo("Some windows lack a reliable receiver DCB; absolute STEC is unavailable there.", err=True)
        click.echo(
            json.dumps({k: summary[k] for k in ("status", "samples", "arcs", "valid_arcs", "calibrated_windows", "windows")})
        )
    except (ValueError, RuntimeError, OSError, KeyError, TypeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
