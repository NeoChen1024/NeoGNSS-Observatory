# SPDX-License-Identifier: GPL-3.0-only
"""Project time axis: continuous seconds since 1980-01-06 00:00:00 GPST.

Naive datetime arithmetic below is calendar formatting, not a UTC conversion.
Never attach a UTC timezone or Z suffix to a GPST label.
"""

from datetime import datetime, timedelta

EPOCH = datetime(1980, 1, 6)


def calendar(seconds):
    return EPOCH + timedelta(seconds=seconds)


def label(seconds):
    return calendar(seconds).strftime("GPST-%Y-%m-%d--%H-%M-%S")


def parse_hour(value):
    return int((datetime.strptime(value, "%Y-%m-%dT%H") - EPOCH).total_seconds())
