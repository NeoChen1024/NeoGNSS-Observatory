# SPDX-License-Identifier: GPL-3.0-only
"""Static forward GPS L1/L2 PPP Float directly from UBX/SBF."""

import datetime as dt
import json
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
from .gpst import EPOCH, calendar
from .ppp_products import Products
from .protocol import ProtocolWarnings, protocol_option
from .research_output import staged_output, write_json


def time_ns(value):
    if value is None:
        return None
    date = dt.datetime.fromisoformat(str(value))
    if date.tzinfo:
        raise ValueError("Observation windows are GPST calendar times without a timezone suffix")
    return int((date - EPOCH).total_seconds() * 1e9)


def position_summary(final):
    x, y, z = (final[k] for k in ("x", "y", "z"))
    longitude = np.arctan2(y, x)
    rho = np.hypot(x, y)
    eccentricity2 = 6.6943799901413165e-3
    latitude = np.arctan2(z, rho * (1 - eccentricity2))
    for _ in range(10):
        radius = 6378137.0 / np.sqrt(1 - eccentricity2 * np.sin(latitude) ** 2)
        latitude = np.arctan2(z + eccentricity2 * radius * np.sin(latitude), rho)
    height = rho * np.cos(latitude) + z * np.sin(latitude) - radius * (1 - eccentricity2 * np.sin(latitude) ** 2)
    sl, cl, sp, cp = np.sin(longitude), np.cos(longitude), np.sin(latitude), np.cos(latitude)
    rotation = np.array([[-sl, cl, 0], [-sp * cl, -sp * sl, cp], [cp * cl, cp * sl, sp]])
    covariance = np.array(
        [
            [final["qxx"], final["qxy"], final["qzx"]],
            [final["qxy"], final["qyy"], final["qyz"]],
            [final["qzx"], final["qyz"], final["qzz"]],
        ]
    )
    enu_covariance = rotation @ covariance @ rotation.T
    values, vectors = np.linalg.eigh(enu_covariance[:2, :2])
    axes = np.sqrt(np.maximum(values, 0) * 5.991464547107979)
    return dict(
        ecef_m=[x, y, z],
        latitude_deg=float(np.rad2deg(latitude)),
        longitude_deg=float(np.rad2deg(longitude)),
        ellipsoidal_height_m=float(height),
        covariance_enu_m2=enu_covariance.tolist(),
        horizontal_ellipse_95=dict(
            semi_major_m=float(axes[1]),
            semi_minor_m=float(axes[0]),
            azimuth_deg=float(np.rad2deg(np.arctan2(vectors[0, 1], vectors[1, 1])) % 180),
        ),
        reference_point="marker",
        definition="last valid forward static filter estimate, not epoch average",
    )


class DailyTables:
    def __init__(self, root, kind, metadata):
        self.root, self.kind, self.metadata = root, kind, metadata
        self.writer = None
        self.day = None
        self.paths = []

    def append(self, array):
        if not len(array):
            return
        days = array["gpst_ns"] // 86_400_000_000_000
        for day in np.unique(days):
            rows = array[days == day]
            table = pa.table({name: rows[name] for name in rows.dtype.names})
            table = table.replace_schema_metadata({b"ngo": json.dumps(self.metadata).encode()})
            if self.day != day:
                self.close()
                self.day = day
                label = (EPOCH + dt.timedelta(days=int(day))).strftime("GPST-%Y-%m-%d")
                path = self.root / self.kind / f"{label}.parquet"
                path.parent.mkdir(parents=True, exist_ok=True)
                if path.exists():
                    raise ValueError("Non-monotonic daily PPP output")
                self.writer = pq.ParquetWriter(path, table.schema, compression="zstd", compression_level=3)
                self.paths.append(str(path.relative_to(self.root)))
            self.writer.write_table(table)

    def close(self):
        if self.writer:
            self.writer.close()
            self.writer = None


@click.command()
@protocol_option
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--config", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option("--start", help="Inclusive GPST calendar time; default first measurement.")
@click.option("--end", help="Exclusive GPST calendar time; default end of stream.")
@click.option("--overwrite", is_flag=True)
@staged_output
def cli(protocol, input_dir, config, output, start, end):
    """Run static GPS Float PPP; no RINEX or external worker is required."""
    try:
        settings = tomllib.loads(config.read_text())
        required = {"station", "position_ecef_m", "antenna_model", "radome", "arp_enu_m", "products_root", "antenna_catalogs"}
        if required - settings.keys():
            raise ValueError(f"Missing PPP settings: {sorted(required-settings.keys())}")
        defaults = dict(signal1="1C", signal2="2L", interval=30.0, gap_timeout=50.0, elevation_deg=7.5)
        native_settings = {key: settings.get(key, value) for key, value in defaults.items()}
        native_settings.update(position_ecef_m=settings["position_ecef_m"], arp_enu_m=settings["arp_enu_m"])
        native_settings["antenna"] = f"{settings['antenna_model']:<16}{settings['radome']:<4}".rstrip()
        start_ns = time_ns(start)
        end_ns = time_ns(end)
        start_ns = 0 if start_ns is None else start_ns
        end_ns = 2**63 - 1 if end_ns is None else end_ns
        if end_ns <= start_ns:
            raise ValueError("End must follow start")

        def resolve(value):
            path = Path(value).expanduser()
            return path.resolve() if path.is_absolute() else (config.resolve().parent / path).resolve()

        products_root = resolve(settings["products_root"])
        catalogs = [resolve(p) for p in settings["antenna_catalogs"]]
        if not catalogs or not all(p.is_file() for p in catalogs):
            raise ValueError("Supply existing IGS20-first ANTEX catalogs; NGS20 may follow")
        if not products_root.is_dir():
            raise ValueError("Missing local product root")
        if output.resolve().is_relative_to(products_root) or products_root.is_relative_to(output.resolve()):
            raise ValueError("PPP output and product input must not contain each other")
        paths = recordings(input_dir.resolve(), protocol, True)
        output.mkdir()
        metadata = dict(
            time_scale="GPST",
            time_epoch="1980-01-06",
            mode="static_forward_float",
            systems="GPS",
            station=settings["station"],
            settings=native_settings,
            units="SI; clock_ns; angles_deg",
            reference_system="IGS20",
            uncertainty="formal 1-sigma",
            residuals="post-fit ionosphere-free L1/L2; meters; NaN when unused",
        )
        tables = [DailyTables(output, name, metadata) for name in ("epochs", "satellites")]
        reader = _native.ObservationReader(protocol)
        solver = _native.PppFloat(native_settings)
        first = last = None
        final = None
        product_windows = []
        try:
            with tempfile.TemporaryDirectory(prefix="ngo-ppp-products-") as scratch:
                products = Products(products_root, scratch, native_settings["antenna"], catalogs, settings.get("margin_hours", 6))
                with tqdm(total=sum(p.stat().st_size for p in paths), desc="PPP Float", unit="B", unit_scale=True) as progress:
                    done = False
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
                                        solver.products(selected)
                                        product_windows.append(selected["metadata"])
                                    rows, satellites = solver.process(batch)
                                    tables[0].append(rows)
                                    tables[1].append(satellites)
                                    if len(rows):
                                        first = int(rows[0]["gpst_ns"]) if first is None else first
                                        last = int(rows[-1]["gpst_ns"])
                                        valid = rows[rows["status"] == 6]
                                        if len(valid):
                                            final = {k: valid[-1][k].item() for k in valid.dtype.names}
                                if reached_end:
                                    done = True
                                    break
                        warnings.update(reader.summary(), final=True)
                        if done:
                            break
                if not done:
                    reader.finish()
        finally:
            for table in tables:
                table.close()
        summary = solver.summary()
        if not summary["solved_epochs"]:
            raise ValueError("No valid PPP Float solutions; incomplete output retained")
        summary.update(
            status="complete",
            station=settings["station"],
            protocol=protocol,
            start_gpst_ns=first,
            end_gpst_ns=last,
            final_epoch_solution=final,
            reader=reader.summary(),
            product_windows=product_windows,
            tables={t.kind: t.paths for t in tables},
        )
        summary["position"] = position_summary(final)
        summary["valid_epoch_fraction"] = summary["solved_epochs"] / summary["attempted_epochs"]
        write_json(output / "summary.json", summary)
        click.echo(json.dumps(dict(status="complete", attempted=summary["attempted_epochs"], solved=summary["solved_epochs"])))
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
