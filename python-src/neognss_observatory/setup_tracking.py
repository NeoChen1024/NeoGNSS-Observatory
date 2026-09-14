# SPDX-License-Identifier: GPL-3.0-only
"""Optional Septentrio config interpretation; never execute receiver commands."""

import re

# mosaic-X5 firmware 4.15.0 Reference Guide, sections 2.2.1, 3 (snt), 4.1.10.
# These are potential measurement codes, not RawBits signal/service identities.
SEPTENTRIO_SIGNALS = {
    "GPSL1CA": ("G", ("1C",)),
    "GPSL1PY": ("G", ("1W",)),
    "GPSL2PY": ("G", ("2W",)),
    "GPSL2C": ("G", ("2L",)),
    "GPSL5": ("G", ("5Q",)),
    "GALE1BC": ("E", ("1C",)),
    "GALE6BC": ("E", ("6B", "6C")),
    "GALE5a": ("E", ("5Q",)),
    "GALE5b": ("E", ("7Q",)),
    "GALE5": ("E", ("8Q",)),
    "GEOL1": ("S", ("1C",)),
    "GEOL5": ("S", ("5I",)),
    "BDSB1I": ("C", ("2I",)),
    "BDSB2I": ("C", ("7I",)),
    "BDSB3I": ("C", ("6I",)),
    "BDSB1C": ("C", ("1P",)),
    "BDSB2a": ("C", ("5P",)),
    "BDSB2b": ("C", ("7D",)),
    "QZSL1CA": ("J", ("1C",)),
    "QZSL2C": ("J", ("2L",)),
    "QZSL5": ("J", ("5Q",)),
    "QZSL1CB": ("J", ("1E",)),
}
SYSTEMS = ("G", "E", "C", "J", "S")
EXCLUDED = {"GLOL1CA", "GLOL2P", "GLOL2CA", "GLOL3", "NAVICL5", "GLONASS", "NAVIC"}


def validate_tracking(tracking):
    """Validate the declaration shape, not a version-specific signal whitelist."""
    if tracking is None:
        return
    if not isinstance(tracking, dict):
        raise ValueError("tracking must be an object or null")
    for system, codes in tracking.items():
        if system not in SYSTEMS:
            raise ValueError("tracking systems must be G, E, C, J or S")
        if codes is None:
            continue
        if not isinstance(codes, list) or any(not isinstance(code, str) or not re.fullmatch(r"[1-9][A-Z]", code) for code in codes):
            raise ValueError("tracking entries must be lists of two-character RINEX signal codes, or null")
        if len(set(codes)) != len(codes):
            raise ValueError("tracking signal lists must not contain duplicates")


def septentrio_tracking(text):
    """Read the last full SignalTracking assignment in a text config dump."""
    selected = None
    for line in text.splitlines():
        fields = [field.strip() for field in line.strip().split(",")]
        if fields[0] not in ("setSignalTracking", "snt", "SignalTracking"):
            continue
        if len(fields) != 2 or not fields[1]:
            raise ValueError("SignalTracking requires an explicit signal list")
        selected = set()
        for token in fields[1].split("+"):
            token = token.strip()
            if token in ("all", "GPS", "GALILEO", "BEIDOU", "QZSS", "SBAS"):
                raise ValueError("SignalTracking aggregate aliases require an expanded signal list for this receiver")
            if token in SEPTENTRIO_SIGNALS:
                selected.add(token)
            elif token not in EXCLUDED:
                # Do not publish partial lists or false disabled-system declarations.
                raise ValueError("Unsupported SignalTracking token; tracking was not inferred")
    if selected is None:
        return None
    # Explicitly enabled but ineffective signals are not promised as measurements.
    if "GPSL1CA" not in selected:
        selected.discard("GPSL2PY")
    if not {"GPSL1CA", "GPSL2PY"} <= selected:
        selected.discard("GPSL1PY")
    result = {system: set() for system in SYSTEMS}
    for token in selected:
        system, codes = SEPTENTRIO_SIGNALS[token]
        result[system].update(codes)
    return {system: sorted(codes) for system, codes in result.items()}


def populate_tracking(setup, config_source, warn):
    """Fill unknown systems, preserving explicit declarations and opaque configs."""
    explicit = setup.get("tracking")
    validate_tracking(explicit)
    if config_source is None:
        return
    receiver = setup.get("receiver") or {}
    if not isinstance(receiver, dict):
        raise ValueError("receiver must be an object or null")
    vendor = receiver.get("vendor")
    if not isinstance(vendor, str) or vendor.casefold() != "septentrio":
        return
    try:
        inferred = septentrio_tracking(config_source.read_text(encoding="utf-8-sig"))
    except (UnicodeError, ValueError) as error:
        # Do not echo arbitrary config lines, which could contain sensitive data.
        message = "Vendor config is not UTF-8 text" if isinstance(error, UnicodeError) else str(error)
        warn(f"{message}; retaining explicit tracking only.")
        return
    if inferred is None:
        warn("No explicit SignalTracking assignment found; retaining explicit tracking only.")
        return
    tracking = dict(explicit or {})
    for system, codes in inferred.items():
        if tracking.get(system) is None:
            tracking[system] = codes
        elif set(tracking[system]) != set(codes):
            warn(f"Explicit tracking.{system} differs from vendor config; keeping the explicit declaration.")
    setup["tracking"] = tracking
