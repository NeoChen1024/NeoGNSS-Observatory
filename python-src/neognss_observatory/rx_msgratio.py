#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Measure UBX/SBF message storage without scientific payload decoding."""

import csv
import json
import sys
from pathlib import Path

import click
from rich.console import Console
from rich.table import Table
from tqdm import tqdm

from . import _native


def machine_report(result, protocol, input_file, output_format):
    """Emit numeric byte counts and percentages without presentation formatting."""
    size = result["source_bytes"]
    messages = [dict(row, percent_of_file=100 * row["bytes"] / size if size else 0.0) for row in result["messages"]]
    if output_format == "json":
        click.echo(
            json.dumps(
                {
                    **result,
                    "protocol": protocol,
                    "input_file": str(input_file),
                    "byte_unit": "byte",
                    "percentage_denominator": "source_bytes",
                    "messages": messages,
                },
                ensure_ascii=False,
                allow_nan=False,
            )
        )
        return
    writer = csv.DictWriter(
        click.get_text_stream("stdout"),
        fieldnames=[
            "record_type",
            "protocol",
            "id",
            "name",
            "revisions",
            "frames",
            "bytes",
            "percent_of_file",
            "source_bytes",
            "invalid_candidates",
            "pending_tail_bytes",
        ],
        lineterminator="\n",
    )
    writer.writeheader()
    for row in messages:
        writer.writerow(
            {
                **row,
                "record_type": "message",
                "protocol": protocol,
                "revisions": ";".join(map(str, row["revisions"])),
                "source_bytes": size,
            }
        )
    for key, name, frames in (
        ("foreign_bytes", "foreign", result["foreign_frames"]),
        ("unclassified_bytes", "unclassified", ""),
    ):
        row = {
            "record_type": name,
            "protocol": protocol,
            "bytes": result[key],
            "frames": frames,
            "percent_of_file": 100 * result[key] / size if size else 0.0,
            "source_bytes": size,
        }
        if name == "unclassified":
            row.update(invalid_candidates=result["invalid_candidates"], pending_tail_bytes=result["pending_tail_bytes"])
        writer.writerow(row)


@click.command()
@click.argument("input_file", type=click.Path(exists=True, dir_okay=False, readable=True, path_type=Path))
@click.option("--protocol", "-p", type=click.Choice(["ubx", "sbf"]), default="ubx", show_default=True)
@click.option(
    "--format",
    "output_format",
    type=click.Choice(["table", "json", "csv"]),
    default="table",
    show_default=True,
    help="Output format on stdout; progress and warnings always use stderr.",
)
def cli(input_file, protocol, output_format):
    """List every checksum-valid message type and its share of INPUT_FILE bytes.

    Read the entire expanded file. Sizes include framing, checksums and padding.
    Group UBX by class/message ID and SBF by block ID (combining revisions).
    Unknown IDs remain visible. Complete foreign-protocol frames are skipped.
    """
    try:
        if input_file.suffix.lower() in (".xz", ".gz", ".zst", ".zip"):
            raise ValueError("Provide an expanded raw file, not a compressed archive")
        label = protocol.upper()
        foreign = "UBX" if protocol == "sbf" else "SBF"
        size = input_file.stat().st_size
        scanner = _native.RxMessageRatio(protocol)
        warned_foreign = False
        with (
            input_file.open("rb") as source,
            tqdm(total=size, desc=f"{label} message sizes", unit="B", unit_scale=True, file=sys.stderr) as progress,
        ):
            remaining = size
            while remaining:
                data = source.read(min(4 * 1024**2, remaining))
                if not data:
                    raise ValueError("Input shortened while reading")
                scanner.feed(data)
                remaining -= len(data)
                progress.update(len(data))
                if not warned_foreign and scanner.summary()["foreign_frames"]:
                    tqdm.write(f"Warning: skipping complete {foreign} frames in {label} input", file=sys.stderr)
                    warned_foreign = True
        if input_file.stat().st_size != size:
            raise ValueError("Input size changed while reading")
        result = scanner.summary()
        result["messages"].sort(key=lambda r: (-r["bytes"], r["id"]))
        if result["unclassified_bytes"] or result["invalid_candidates"]:
            click.echo(
                f"Warning: {result['invalid_candidates']} invalid framing/checksum candidates; "
                f"{result['unclassified_bytes']} unclassified bytes (including {result['pending_tail_bytes']} pending tail bytes). "
                "These bytes are not attributed to message IDs.",
                err=True,
            )
        if output_format != "table":
            machine_report(result, protocol, input_file, output_format)
            return
        table = Table(title=f"{label} message sizes: {input_file.name}")
        for name in ("Block ID" if protocol == "sbf" else "Class/ID", "Message", "Revisions", "Count", "Bytes", "% of file"):
            table.add_column(name, justify="left" if name == "Message" else "right", no_wrap=name != "Message", overflow="fold")

        def share(n):
            return f"{100 * n / size:.4f}%" if size else "0.0000%"

        for row in result["messages"]:
            table.add_row(
                str(row["id"]) if protocol == "sbf" else f"{row['id'] >> 8:02X}/{row['id'] & 255:02X}",
                row["name"],
                ",".join(map(str, row["revisions"])) if protocol == "sbf" else "-",
                f"{row['frames']:,}",
                f"{row['bytes']:,}",
                share(row["bytes"]),
            )
        table.add_section()
        table.add_row(
            "",
            f"All valid {label}",
            "",
            f"{sum(r['frames'] for r in result['messages']):,}",
            f"{result['valid_bytes']:,}",
            share(result["valid_bytes"]),
        )
        for key, label in (("foreign_bytes", f"Foreign {foreign}"), ("unclassified_bytes", "Unclassified / damaged / tail")):
            if result[key]:
                table.add_row("", label, "", "", f"{result[key]:,}", share(result[key]))
        console = Console()
        console.print(table)
        console.print(f"File size: {size:,} bytes. Shares use the entire file as denominator.")
    except (OSError, ValueError, RuntimeError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
