# SPDX-License-Identifier: GPL-3.0-only
"""Select complete receiver calibration records without rewriting ANTEX data."""

import gzip
from datetime import datetime
from decimal import Decimal, InvalidOperation


def label(line):
    return line[60:80].strip()


def timestamp(line):
    fields = line[:60].split()
    if len(fields) != 6:
        raise ValueError("Invalid ANTEX validity timestamp")
    dt = datetime(*map(int, fields[:5]))
    try:
        seconds = Decimal(fields[5].decode("ascii"))
    except InvalidOperation as error:
        raise ValueError("Invalid ANTEX validity seconds") from error
    if not seconds.is_finite() or not 0 <= seconds < 60:
        raise ValueError("Invalid ANTEX validity seconds")
    delta = dt - datetime(1980, 1, 6)
    return Decimal(delta.days * 86400 + delta.seconds) + seconds


def records(path):
    """Read an ANTEX 1.4 absolute catalog as byte-preserved header and records."""
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    with opener(path, "rb") as source:
        header = []
        for line in source:
            header.append(line)
            if label(line) == b"END OF HEADER":
                break
        else:
            raise ValueError(f"Missing ANTEX header: {path}")
        if not header or label(header[0]) != b"ANTEX VERSION / SYST" or header[0][:8].strip() != b"1.4":
            raise ValueError(f"Expected ANTEX 1.4: {path}")
        pcv = [line for line in header if label(line) == b"PCV TYPE / REFANT"]
        if len(pcv) != 1 or pcv[0][:1] != b"A":
            raise ValueError(f"Expected absolute ANTEX calibration, not relative/unknown: {path}")
        block = None
        for line in source:
            tag = label(line)
            if tag == b"START OF ANTENNA":
                if block is not None:
                    raise ValueError(f"Nested ANTEX antenna block: {path}")
                block = []
            if block is not None:
                block.append(line)
            elif line.strip():
                raise ValueError(f"Unexpected data outside ANTEX antenna block: {path}")
            if tag == b"END OF ANTENNA":
                if block is None:
                    raise ValueError(f"Unmatched ANTEX antenna end: {path}")
                yield b"".join(header), block
                block = None
        if block is not None:
            raise ValueError(f"Truncated ANTEX antenna block: {path}")


def validate_block(block):
    counts = [line for line in block if label(line) == b"# OF FREQUENCIES"]
    if len(counts) != 1:
        raise ValueError("Selected ANTEX record requires a frequency count")
    starts = [line[:10].strip() for line in block if label(line) == b"START OF FREQUENCY"]
    ends = [line[:10].strip() for line in block if label(line) == b"END OF FREQUENCY"]
    if not starts or starts != ends or len(starts) != int(counts[0][:6]) or len(set(starts)) != len(starts):
        raise ValueError("Incomplete or duplicate frequency records in selected ANTEX calibration")
    times = {}
    for line in block:
        tag = label(line)
        if tag in (b"VALID FROM", b"VALID UNTIL"):
            if tag in times:
                raise ValueError("Duplicate ANTEX validity bound")
            times[tag] = timestamp(line)
    start = times.get(b"VALID FROM", Decimal("-Infinity"))
    end = times.get(b"VALID UNTIL", Decimal("Infinity"))
    if start > end:
        raise ValueError("Reversed ANTEX validity interval")
    return start, end


def select_antenna(catalogs, antenna):
    """Prefer matching individual calibration, otherwise first catalog's type mean."""
    model, radome = antenna.get("type"), antenna.get("radome")
    if not model or not radome:
        raise ValueError("ANTEX selection requires antenna.type and explicit antenna.radome (unknown is not NONE)")
    model, radome = model.encode("ascii"), radome.encode("ascii")
    serial = (antenna.get("serial_number") or "").encode("ascii")
    if len(model) > 15 or len(radome) != 4 or model.strip() != model or radome.strip() != radome:
        raise ValueError("ANTEX selection requires a standard antenna type and four-character radome code")
    generic = None
    for path in catalogs:
        matches = {"individual": [], "generic": []}
        header = None
        for record_header, block in records(path):
            identities = [line for line in block if label(line) == b"TYPE / SERIAL NO"]
            if len(identities) != 1:
                raise ValueError(f"ANTEX record requires one TYPE / SERIAL NO: {path}")
            identity = identities[0]
            if identity[:16].rstrip() != model or identity[16:20] != radome:
                continue
            record_serial = identity[20:40].strip()
            if record_serial and record_serial != serial:
                continue
            kind = "individual" if record_serial else "generic"
            start, end = validate_block(block)
            matches[kind].append((start, end, b"".join(block)))
            header = record_header
        selected = matches["individual"] or matches["generic"]
        if not selected:
            continue
        # Byte-identical duplicates need not create ambiguity or redundant records.
        selected = sorted(set(selected), key=lambda item: (item[0], item[1]))
        for previous, current in zip(selected, selected[1:]):
            if current[0] <= previous[1]:
                raise ValueError(f"Ambiguous overlapping ANTEX calibration records: {path}")
        result = header + b"".join(item[2] for item in selected)
        if matches["individual"]:
            return result, path, len(selected), "individual"
        if generic is None:
            generic = result, path, len(selected), "type-mean"
    if generic is not None:
        return generic
    raise ValueError("No matching antenna/radome type-mean or matching-serial calibration in supplied catalogs")
