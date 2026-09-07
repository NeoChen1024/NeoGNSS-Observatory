# SPDX-License-Identifier: GPL-3.0-only
"""Explicit raw protocol selection and bounded terminal diagnostics."""

import sys

import click
from tqdm import tqdm


def protocol_option(function):
    return click.option(
        "--protocol",
        "-p",
        type=click.Choice(["ubx", "sbf"], case_sensitive=False),
        default="ubx",
        show_default=True,
        help="Parse only this wire protocol; warn and skip valid frames of the other protocol.",
    )(function)


def require_ubx(protocol, operation):
    if protocol != "ubx":
        raise click.ClickException(
            f"{operation} currently supports UBX only; use python -m neognss_observatory.sbas_extract -p sbf for SBF extraction."
        )


class ProtocolWarnings:
    """Warn at the first skipped batch per file; summarize further skips at EOF."""

    def __init__(self, source, protocol, initial=None):
        self.source, self.other = source, "SBF" if protocol == "ubx" else "UBX"
        self.base = (initial or {}).get("skipped_protocol_frames", 0)
        self.reported = 0

    def update(self, summary, *, final=False):
        count = summary.get("skipped_protocol_frames", 0) - self.base
        if count > self.reported and (not self.reported or final):
            tqdm.write(f"Warning: {self.source}: skipped {count} {self.other} frame(s) (unselected protocol).", file=sys.stderr)
            self.reported = count
