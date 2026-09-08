# SPDX-License-Identifier: GPL-3.0-only
"""Extract reconstructed SBAS streams without resetting at continuous file boundaries."""

import json
from pathlib import Path

import click
from tqdm import tqdm

from . import _native
from .protocol import ProtocolWarnings, protocol_option
from .research_output import staged_output, write_json


def extract_ubx(root, sink, gap_timeout):
    from .dataset_inputs import recordings
    from .sbas_frames import FrameStreams

    paths = recordings(root)
    streams = FrameStreams(sink)
    parser = _native.DatasetScan(protocol="ubx", qa=False, gap_timeout=gap_timeout)

    def consume(rows):
        for row in rows:
            if row["kind"] == "end":
                streams.finish(row["gpst_ms"])
            else:
                streams.add(row["prn"], row["gpst_ms"], row["sbas"], "navigation_epoch_context")

    with tqdm(total=sum(p.stat().st_size for p in paths), desc="Extract SBAS", unit="B", unit_scale=True) as progress:
        for path in paths:
            warnings = ProtocolWarnings(path, "ubx", parser.summary())
            with path.open("rb") as source:
                while block := source.read(4 * 1024 * 1024):
                    consume(parser.feed(block))
                    warnings.update(parser.summary())
                    progress.update(len(block))
            warnings.update(parser.summary(), final=True)
    consume(parser.finish())
    return parser.summary()


@click.command()
@protocol_option
@click.option("--input-dir", type=click.Path(exists=True, file_okay=False, path_type=Path), required=True)
@click.option("--output", type=click.Path(path_type=Path), required=True)
@click.option(
    "--gap-timeout",
    type=click.FloatRange(min=0.001),
    default=50,
    show_default=True,
    help="Gap in seconds: UBX navigation epochs, SBF per-signal reception.",
)
@click.option("--overwrite", is_flag=True, help="Replace output after success; retain the previous directory as a backup.")
@staged_output
def cli(input_dir, output, protocol="ubx", gap_timeout=50):
    """Extract source-independent SBAS frame Parquet, partitioned by GPST day."""
    from .sbas_frames import FrameSink

    try:
        output.mkdir()
        sink = FrameSink(output)
        if protocol == "sbf":
            from .sbf_extract import extract

            diagnostics = extract(input_dir.resolve(), sink, round(gap_timeout * 1000))
        else:
            diagnostics = extract_ubx(input_dir.resolve(), sink, gap_timeout)
        days = sink.finish()
        result = dict(status="complete", product="sbas_frames", days=days, frames=sink.frames, diagnostics=diagnostics)
        write_json(output / "completed.json", result)
        click.echo(json.dumps(result))
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
