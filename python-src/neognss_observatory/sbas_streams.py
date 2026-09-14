# SPDX-License-Identifier: GPL-3.0-only
"""Canonical SBAS signal identity shared by grid and plotting products."""

import re

import click

IDENTITY = ("satellite_system", "satellite_number", "signal")


def sbas_satellite(ctx, param, value):
    value = value.upper()
    if not re.fullmatch(r"S(?:[2-4][0-9]|5[0-8])", value):
        raise click.BadParameter("Use a RINEX SBAS satellite identifier, S20 through S58")
    return value
