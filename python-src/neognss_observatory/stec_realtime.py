# SPDX-License-Identifier: GPL-3.0-only
"""CommonNEX Arrow consumer and bounded live relative-STEC example."""

import json
from contextlib import nullcontext
from dataclasses import dataclass
from decimal import ROUND_HALF_EVEN, Decimal
from pathlib import Path

import click
import numpy as np
import pyarrow as pa

from . import _native
from .cnex_import import open_input
from .cnex_stream import CnexStream, tcp_groups
from .setup_metadata import validate_setup
from .stec import NATIVE_DEFAULTS, antenna_position
from .stec_pairs import build_pairs


def nanoseconds(t):
    return int((t * 10**9).to_integral_value(rounding=ROUND_HALF_EVEN))


@dataclass(frozen=True)
class RealtimeStecResult:
    epochs_ns: tuple[int, ...]
    samples: pa.RecordBatch
    segment: int


class RealtimeStec:
    """Single-owner Arrow consumer; no receiver protocol parsing or file I/O.

    Feed complete CommonNEX groups, including observation-completion Events.
    Other consumers may read the same immutable batches independently.
    """

    def __init__(self, setup, *, elevation_deg=10.0, navigation=None):
        self.setup = validate_setup(setup)
        pairs, models, _ = build_pairs(Path("."), self.setup, "none", {})
        self.pairs = [p | dict(id=i) for i, p in enumerate(p for p in pairs if p["system"] in ("G", "J"))]
        self.navigation = navigation or _native.BroadcastNavigation(self.setup["setup_id"])
        self.segment = 0
        self.settings = NATIVE_DEFAULTS | dict(
            interval=1e-9,
            elevation_deg=elevation_deg,
            level_elevation_deg=max(30.0, elevation_deg),
            position_ecef_m=antenna_position(self.setup),
            pairs=self.pairs,
            antennas=models,
            antenna_required=False,
        )
        self._reset_processor()

    def _reset_processor(self):
        self.reader = _native.StecCnexReader(self.setup["setup_id"], self.pairs)
        self.processor = _native.StecProcessor(self.settings)
        self.processor.navigation(self.navigation)

    def process(self, *, observations=(), raw_bits=(), events=()):
        """Consume Arrow batches; return Arrow samples plus completed epoch times."""

        def batches(value):
            if isinstance(value, pa.RecordBatch):
                return (value,)
            if isinstance(value, pa.Table):
                return value.to_batches()
            return value

        observations, raw_bits, events = (batches(v) for v in (observations, raw_bits, events))
        # Cache entries retain availability timestamps, so later data in the
        # same group cannot be used by earlier observation epochs.
        for batch in raw_bits:
            self.navigation.feed(batch)
        complete = -1
        completed_epochs = []
        restart = []
        for batch in events:
            for row in batch.select(["setup_id", "kind", "scope", "gpst", "payload"]).to_pylist():
                if row["setup_id"] != self.setup["setup_id"]:
                    raise ValueError("Event Setup mismatch")
                if row["kind"] == "RECEIVER_RESTART":
                    if row["gpst"] is None:
                        raise ValueError("Cannot place untimed restart in realtime STEC")
                    restart.append(nanoseconds(row["gpst"]))
                elif row["kind"] == "EPOCH_COMPLETION":
                    if row["scope"] == "OBSERVATION":
                        if row["gpst"] is None or row["payload"]["epoch_completion"]["completion"] != "COMPLETE":
                            raise ValueError("Expected timed complete observation epoch")
                        complete = max(complete, nanoseconds(row["gpst"]))
                        completed_epochs.append(nanoseconds(row["gpst"]))
                else:
                    raise ValueError(f"Unsupported STEC Event: {row['kind']}")
        if restart:
            self.processor.restarts(restart)
        parts, epochs = [], []

        def consume(batch):
            epochs.extend(batch.epochs_ns)
            samples, _ = self.processor.process(batch)
            parts.append(samples)

        for batch in observations:
            consume(self.reader.feed(batch))
        consume(self.reader.complete(complete))
        samples = np.concatenate(parts)
        fields = dict(
            gpst_ns=pa.array(samples["gpst_ns"]),
            satellite_system=pa.array([chr(s) for s in samples["system"]], type=pa.string()),
            satellite_number=pa.array(samples["prn"], type=pa.uint16()),
            pair_id=pa.array(samples["pair_id"]),
            arc_id=pa.array(samples["arc_id"]),
            relative_stec_tecu=pa.array(samples["relative_stec_tecu"]),
            ipp_latitude_deg=pa.array(samples["ipp_latitude_deg"]),
            ipp_longitude_deg=pa.array(samples["ipp_longitude_deg"]),
            elevation_deg=pa.array(samples["elevation_deg"]),
            azimuth_deg=pa.array(samples["azimuth_deg"]),
        )
        return RealtimeStecResult(tuple(sorted(set(epochs) | set(completed_epochs))), pa.record_batch(fields), self.segment)

    def feed(self, group):
        result = self.process(**{k.replace("-", "_"): group.catalogs.get(k, ()) for k in ("observations", "raw-bits", "events")})
        if group.notice and group.notice.startswith("discontinuity:"):
            self.navigation.clear()
            self._reset_processor()
            self.segment += 1
        return result

    def json_epochs(self, result):
        grouped = {}
        for row in result.samples.to_pylist():
            stamp = row.pop("gpst_ns")
            pair = self.pairs[row.pop("pair_id")]
            system, number = row.pop("satellite_system"), row.pop("satellite_number")
            row.update(satellite=f"{system}{number:02d}", signals=[pair["signal1"], pair["signal2"]])
            grouped.setdefault(stamp, []).append(row)
        for stamp in result.epochs_ns:
            yield dict(
                setup_id=self.setup["setup_id"],
                segment=result.segment,
                gpst=f"{Decimal(stamp) / 10**9:.12f}",
                orbit="broadcast_lnav",
                phase="receiver_exported",
                ipp_shell_radius_m=6821000,
                samples=grouped.get(stamp, []),
            )


def file_groups(path, stream):
    with open_input(path) as source:
        while chunk := source.read(65536):
            stream.feed(chunk)
            group = stream.drain()
            if group is not None:
                yield group
    yield stream.finish()


@click.command()
@click.option("--host", help="Receiver TCP hostname or IPv6 address without brackets.")
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
@click.option("--duration", type=click.FloatRange(min=0, min_open=True), help="TCP capture duration in seconds; default unlimited.")
@click.option(
    "--elevation-deg", type=click.FloatRange(min=0, max=90, min_open=True, max_open=True), default=10.0, show_default=True
)
@click.option("--output", type=click.Path(path_type=Path), help="Exclusive-create JSONL file; default stdout.")
def cli(host, port, input_path, protocol, setup_path, max_latency, duration, elevation_deg, output):
    """Emit per-epoch GPS/QZSS IPP and arc-relative STEC JSONL, without CDDIS."""
    if bool(host) == bool(input_path):
        raise click.UsageError("Specify exactly one of --host or --input")
    if input_path and duration is not None:
        raise click.UsageError("--duration applies only to TCP")
    try:
        setup = validate_setup(json.loads(setup_path.read_text()))
        consumer = RealtimeStec(setup, elevation_deg=elevation_deg)
        seconds, fraction = setup["epoch_period_s"].split(".")
        stream = CnexStream(protocol, setup["setup_id"], int(seconds), int(fraction))
        groups = (
            file_groups(input_path, stream)
            if input_path
            else tcp_groups(host, port, stream, max_latency=max_latency, duration=duration)
        )
        click.echo(
            "Relative phase STEC; GPS/QZSS LNAV geometry; no DCB/antenna correction. Waiting for usable ephemerides.", err=True
        )
        with output.open("x") if output else nullcontext(click.get_text_stream("stdout")) as target:
            for group in groups:
                if group is None:
                    continue
                result = consumer.feed(group)
                for row in consumer.json_epochs(result):
                    target.write(json.dumps(row, allow_nan=False) + "\n")
                target.flush()
                if group.notice:
                    click.echo(group.notice, err=True)
        click.echo(f"Decoded ephemerides: {consumer.navigation.decoded}", err=True)
    except (OSError, ValueError, RuntimeError, BufferError, EOFError, OverflowError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
