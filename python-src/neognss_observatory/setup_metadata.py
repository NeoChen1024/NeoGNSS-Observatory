# SPDX-License-Identifier: GPL-3.0-only
"""Single-station Setup metadata and basic scientific field validation."""

import math
import re
from decimal import Decimal

from .setup_tracking import validate_tracking


def component(value):
    if not isinstance(value, str) or not value or value in (".", "..") or any(c in value for c in ("/", "\\", "\0")):
        raise ValueError("Companion filenames must be nonempty single path components")
    return value


def validate_setup(setup):
    if not isinstance(setup, dict):
        raise ValueError("Setup must be a JSON object")
    if not isinstance(setup.get("setup_id"), str) or not setup["setup_id"]:
        raise ValueError("setup_id must be a nonempty free-form string")
    if any(key in setup for key in ("antennas", "antenna_name", "stream_id")):
        raise ValueError("Use a single antenna object; Stream/named-antenna metadata is not supported")

    def obj(value, path, required=False):
        if value is None and not required:
            return {}
        if not isinstance(value, dict):
            raise ValueError(f"{path} must be an object" + ("" if required else " or null"))
        return value

    def strings(value, names, path):
        for name in names:
            if value.get(name) is not None and not isinstance(value[name], str):
                raise ValueError(f"{path}{name} must be a string or null")

    def number(value, path):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError(f"{path} must be a finite number")

    def vector(value, keys, path):
        if value is None:
            return
        value = obj(value, path)
        if set(value) != set(keys):
            raise ValueError(f"{path} requires exactly {', '.join(keys)}; use null when unknown")
        for key in keys:
            number(value[key], f"{path}.{key}")

    strings(setup, ("comment", "observer", "agency", "vendor_config"), "")
    marker = obj(setup.get("marker"), "marker")
    strings(marker, ("name", "number", "type", "reference_frame", "position_basis"), "marker.")
    if marker.get("position_basis") not in (None, "approximate", "surveyed"):
        raise ValueError("marker.position_basis must be approximate, surveyed or null")
    vector(marker.get("position_xyz_m"), ("x", "y", "z"), "marker.position_xyz_m")
    receiver = obj(setup.get("receiver"), "receiver")
    strings(receiver, ("vendor", "model", "rinex_name", "serial_number", "firmware_version", "comment"), "receiver.")
    antenna = obj(setup.get("antenna"), "antenna", required=True)
    strings(antenna, ("type", "radome", "serial_number", "comment", "calibration_file"), "antenna.")
    vector(antenna.get("arp_offset_neu_m"), ("north", "east", "up"), "antenna.arp_offset_neu_m")
    azimuth = antenna.get("azimuth_deg")
    if azimuth is not None:
        number(azimuth, "antenna.azimuth_deg")
        if not 0 <= azimuth < 360:
            raise ValueError("antenna.azimuth_deg must be in [0, 360)")
    feed = obj(antenna.get("feed_line"), "antenna.feed_line")
    strings(feed, ("type",), "antenna.feed_line.")
    if feed.get("length_m") is not None:
        number(feed["length_m"], "antenna.feed_line.length_m")
        if feed["length_m"] < 0:
            raise ValueError("antenna.feed_line.length_m must be nonnegative")
    period = setup.get("epoch_period_s")
    if not isinstance(period, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]{1,12})?", period):
        raise ValueError("epoch_period_s is required and must be a decimal seconds string")
    if not 0 < Decimal(period) < Decimal(10) ** 26:
        raise ValueError("epoch_period_s outside Duration domain")
    setup["epoch_period_s"] = f"{Decimal(period):.12f}"
    validate_tracking(setup.get("tracking"))
    for filename in (setup.get("vendor_config"), antenna.get("calibration_file")):
        if filename is not None:
            component(filename)
    return setup
