# SPDX-License-Identifier: GPL-3.0-only
"""Project time axis: continuous seconds since 1980-01-06 00:00:00 GPST.

Naive datetime arithmetic below is calendar formatting, not a UTC conversion.
Never attach a UTC timezone or Z suffix to a GPST label.
"""

from datetime import datetime, timedelta
from decimal import ROUND_HALF_EVEN, Decimal, localcontext

EPOCH = datetime(1980, 1, 6)
EPOCH_DATE = EPOCH.date()
HOUR_NS = 3600 * 10**9
DAY_NS = 86400 * 10**9


def calendar(seconds):
    return EPOCH + timedelta(seconds=seconds)


def label(seconds):
    return calendar(seconds).strftime("GPST-%Y-%m-%d--%H-%M-%S")


def label_ms(milliseconds):
    seconds, fraction = divmod(milliseconds, 1000)
    return f"{label(seconds)}-{fraction:03d}"


def decimal_ns(seconds):
    """Exact (Decimal or int) GPST seconds as integer nanoseconds, rounded half to even."""
    # decimal128(38,12) needs up to 38 digits; the default context keeps 28.
    with localcontext(prec=50):
        return int((Decimal(seconds) * 10**9).to_integral_value(rounding=ROUND_HALF_EVEN))


def decimal_ticks(seconds):
    """Exact native (seconds, picoseconds) of a decimal128(38,12) or integer GPST value."""
    with localcontext(prec=50):
        return divmod(int(Decimal(seconds).scaleb(12)), 10**12)


def parse_hour(value):
    return int((datetime.strptime(value, "%Y-%m-%dT%H") - EPOCH).total_seconds())
