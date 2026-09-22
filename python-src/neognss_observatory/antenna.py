# SPDX-License-Identifier: GPL-3.0-only
"""Receiver ANTEX models and bounded, non-recursive frequency substitution."""

import math
from decimal import ROUND_CEILING, ROUND_FLOOR, Decimal

from .setup_antex import label, select_antenna, validate_block

# ANTEX frequency identifiers, not receiver channel/RTKLIB slot numbers.
# GLONASS FDMA and NavIC are outside Observatory's scientific scope.
FREQUENCIES_HZ = {
    "G01": 1575420000,
    "G02": 1227600000,
    "G05": 1176450000,
    "E01": 1575420000,
    "E05": 1176450000,
    "E06": 1278750000,
    "E07": 1207140000,
    "E08": 1191795000,
    "C01": 1575420000,
    "C02": 1561098000,
    "C05": 1176450000,
    "C06": 1268520000,
    "C07": 1207140000,
    "C08": 1191795000,
    "J01": 1575420000,
    "J02": 1227600000,
    "J05": 1176450000,
    "J06": 1278750000,
    "S01": 1575420000,
    "S05": 1176450000,
}
MAX_FREQUENCY_DISTANCE_HZ = 25_000_000


def numbers(data):
    values = [float(v) for v in data.split()]
    if not all(math.isfinite(v) for v in values):
        raise ValueError("Non-finite ANTEX calibration")
    return values


def parse_patterns(block):
    """Retain original PCO/PCV only; RMS blocks are not phase corrections."""

    def unique(tag):
        rows = [line for line in block if label(line) == tag]
        if len(rows) != 1:
            raise ValueError(f"Expected one ANTEX {tag.decode()} record")
        return numbers(rows[0][:60])

    zen = unique(b"ZEN1 / ZEN2 / DZEN")
    dazi = unique(b"DAZI")
    if len(zen) != 3 or len(dazi) != 1:
        raise ValueError("Invalid ANTEX angular grid")
    first, last, step = zen
    if not 0 <= first < last <= 90 or step <= 0 or not 0 <= dazi[0] <= 360:
        raise ValueError("Unsupported receiver ANTEX angular grid")
    count = (last - first) / step
    if not math.isfinite(count) or count > 10000 or abs(count - round(count)) > 1e-8:
        raise ValueError("Invalid ANTEX zenith spacing")
    count = round(count) + 1
    patterns, current, rms = {}, None, False
    for row in block:
        tag = label(row)
        if tag == b"START OF FREQ RMS":
            rms = True
        elif tag == b"END OF FREQ RMS":
            rms = False
        elif rms:
            continue
        elif tag == b"START OF FREQUENCY":
            if current is not None:
                raise ValueError("Nested ANTEX frequency block")
            current = row[:10].strip().decode("ascii")
            if current in patterns:
                raise ValueError("Duplicate ANTEX frequency")
            patterns[current] = dict(zenith_deg=zen, dazi_deg=dazi[0], azimuth_pcv_m=[])
        elif tag == b"END OF FREQUENCY":
            if current != row[:10].strip().decode("ascii"):
                raise ValueError("Mismatched ANTEX frequency end")
            current = None
        elif current is not None:
            p = patterns[current]
            if tag == b"NORTH / EAST / UP":
                if "pco_neu_m" in p:
                    raise ValueError("Duplicate ANTEX PCO")
                p["pco_neu_m"] = [v * 0.001 for v in numbers(row[:60])]
            elif row[:8].strip() == b"NOAZI":
                if "noazi_pcv_m" in p:
                    raise ValueError("Duplicate ANTEX NOAZI")
                p["noazi_pcv_m"] = [v * 0.001 for v in numbers(row[8:])]
            elif not tag or row[:8].strip().replace(b".", b"", 1).isdigit():
                # PCV rows extend beyond column 60; never interpret their tail as a label.
                vals = numbers(row)
                if vals:
                    p["azimuth_pcv_m"].append([vals[0], *[v * 0.001 for v in vals[1:]]])
            elif tag != b"COMMENT":
                raise ValueError(f"Unsupported ANTEX frequency record: {tag!r}")
    if current is not None:
        raise ValueError("Truncated ANTEX frequency")
    for p in patterns.values():
        if len(p.get("pco_neu_m", [])) != 3 or len(p.get("noazi_pcv_m", [])) != count:
            raise ValueError("Incomplete ANTEX PCO/NOAZI grid")
        rows = p["azimuth_pcv_m"]
        if dazi[0]:
            steps = 360 / dazi[0]
            if not math.isfinite(steps) or steps > 10000 or abs(steps - round(steps)) > 1e-8 or len(rows) != round(steps) + 1:
                raise ValueError("Incomplete ANTEX azimuth grid")
            if any(len(r) != count + 1 or abs(r[0] - i * dazi[0]) > 1e-6 for i, r in enumerate(rows)):
                raise ValueError("Invalid ANTEX azimuth row")
            if rows[0][1:] != rows[-1][1:]:
                raise ValueError("Discontinuous ANTEX 0/360 degree seam")
        elif rows:
            raise ValueError("ANTEX azimuth rows with zero DAZI")
    return {k: v for k, v in patterns.items() if k in FREQUENCIES_HZ}


def resolve_frequency(patterns, target):
    """Resolve using original records only; distances are absolute Hz."""
    if target not in FREQUENCIES_HZ:
        raise ValueError(f"Unsupported receiver antenna frequency: {target}")
    frequency = FREQUENCIES_HZ[target]
    if target in patterns:
        sources, weights, method = [target], [1.0], "native"
    else:
        groups = {}
        for code in patterns:
            groups.setdefault(FREQUENCIES_HZ[code], []).append(code)

        def source(hz):
            codes = sorted(groups[hz])
            preferred = [code for code in codes if code[0] == target[0]]
            if preferred:
                return preferred[0]
            if any(patterns[code] != patterns[codes[0]] for code in codes[1:]):
                raise ValueError(f"Ambiguous same-frequency ANTEX sources for {target}: {codes}")
            return codes[0]

        if frequency in groups:
            sources, weights, method = [source(frequency)], [1.0], "same-frequency"
        else:
            nearby = sorted(hz for hz in groups if abs(hz - frequency) <= MAX_FREQUENCY_DISTANCE_HZ)
            lower = [hz for hz in nearby if hz < frequency]
            upper = [hz for hz in nearby if hz > frequency]
            if lower and upper:
                lo, hi = lower[-1], upper[0]
                w = (frequency - lo) / (hi - lo)
                sources, weights, method = [source(lo), source(hi)], [1 - w, w], "interpolated"
            elif nearby:
                distance = min(abs(hz - frequency) for hz in nearby)
                closest = [hz for hz in nearby if abs(hz - frequency) == distance]
                if len(closest) != 1:
                    raise ValueError(f"Equidistant ANTEX sources for {target}")
                sources, weights, method = [source(closest[0])], [1.0], "near-frequency substitution"
            else:
                raise ValueError(f"No receiver calibration within 25 MHz for {target} ({frequency} Hz)")
    return dict(
        target=target,
        frequency_hz=frequency,
        method=method,
        sources=[
            dict(
                code=code,
                frequency_hz=FREQUENCIES_HZ[code],
                delta_hz=frequency - FREQUENCIES_HZ[code],
                weight=weight,
                pattern=patterns[code],
            )
            for code, weight in zip(sources, weights)
        ],
    )


def receiver_model(catalogs, antenna, targets):
    """Select one catalog/serial family and resolve every validity record."""
    data, path, _, kind = select_antenna(catalogs, antenna)
    azimuth = antenna.get("azimuth_deg")
    if azimuth is not None and (not math.isfinite(azimuth) or not 0 <= azimuth < 360):
        raise ValueError("Antenna azimuth_deg must be in [0, 360)")
    result = dict(
        version=1,
        source=str(path),
        selection=kind,
        azimuth_deg=azimuth,
        max_frequency_distance_hz=MAX_FREQUENCY_DISTANCE_HZ,
        records=[],
    )
    block = None
    for line in data.splitlines():
        if label(line) == b"START OF ANTENNA":
            block = []
        if block is not None:
            block.append(line)
        if label(line) == b"END OF ANTENNA":
            start, end = validate_block(block)
            patterns = parse_patterns(block)
            result["records"].append(
                dict(
                    start_ns=(
                        max(0, int((start * Decimal(10**9)).to_integral_value(rounding=ROUND_CEILING))) if start.is_finite() else 0
                    ),
                    end_ns=(
                        min(2**63 - 1, int((end * Decimal(10**9)).to_integral_value(rounding=ROUND_FLOOR)))
                        if end.is_finite()
                        else 2**63 - 1
                    ),
                    frequencies=[resolve_frequency(patterns, target) for target in targets],
                )
            )
            block = None
    return result


def model_metadata(model):
    """Compact scientific selection information, without numerical PCV arrays."""
    return {
        **model,
        "records": [
            {
                **record,
                "frequencies": [
                    {**freq, "sources": [{k: v for k, v in source.items() if k != "pattern"} for source in freq["sources"]]}
                    for freq in record["frequencies"]
                ],
            }
            for record in model["records"]
        ],
    }
