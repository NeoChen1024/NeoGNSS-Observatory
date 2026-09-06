"""Exact overlap proofs and GPST segment plans for UBX archives."""

from pathlib import Path

from tqdm import tqdm

from .ubx_restitch import (
    CONFLICT,
    EOE,
    NOISE,
    PARTIAL,
    PVT,
    RAWX,
    UNKNOWN,
    epochs,
    gpst_label,
)


def equal_ranges(left, left_begin, right, right_begin, size):
    """Compare actual bytes, never treating a fingerprint as proof."""
    if min(left_begin, right_begin, size) < 0:
        return False
    with Path(left).open("rb") as a, Path(right).open("rb") as b:
        a.seek(left_begin)
        b.seek(right_begin)
        while size:
            n = min(size, 1024 * 1024)
            data = a.read(n)
            if len(data) != n or data != b.read(n):
                return False
            size -= n
    return True


def useful_rows(source):
    return [e for e in epochs(Path(source["index"])) if not e.flags & NOISE]


def join_sources(previous, incoming):
    """Find two adjacent exact EOE intervals and prove both discarded sides."""
    old = [e for e in useful_rows(previous) if e.begin >= previous["begin"] and e.end <= previous["end"]]
    new = useful_rows(incoming)
    # Daily logger rotation can put RAWX/SFRBX in the outgoing file and
    # NAV-PVT through NAV-EOE for that same epoch in the incoming file.
    # Accept only complementary, physically adjacent boundary intervals.
    a, b = old[-1], new[0]
    if (
        a.gpst == b.gpst != UNKNOWN
        and a.tow == b.tow
        and previous["last_gpst"] == incoming["first_gpst"]
        and a.end == previous["size"]
        and b.begin == 0
        and a.flags & PARTIAL
        and not a.flags & (EOE | CONFLICT)
        and a.nav == 0
        and a.week >= 0
        and b.flags & PVT
        and b.flags & EOE
        and not b.flags & (PARTIAL | CONFLICT)
        and b.nav > 0
        and not b.flags & RAWX
    ):
        return {
            "previous": previous["path"],
            "incoming": incoming["path"],
            "anchor_gpst": b.gpst,
            "previous_cut": previous["end"],
            "incoming_cut": incoming["begin"],
            "proof": "complementary same-GPST/iTOW RAWX tail and NAV-PVT/EOE head; all bytes retained",
            "kind": "split_epoch_continuation",
        }
    lookup = {(e.gpst, e.tow, e.fingerprint, e.end - e.begin): i for i, e in enumerate(old) if e.flags & EOE}
    for j in range(1, len(new)):
        b0, b1 = new[j - 1 : j + 1]
        if not (b0.flags & EOE and b1.flags & EOE) or b0.end != b1.begin or b1.gpst != b0.gpst + 1:
            continue
        i = lookup.get((b1.gpst, b1.tow, b1.fingerprint, b1.end - b1.begin), -1)
        if i < 1:
            continue
        a0, a1 = old[i - 1 : i + 1]
        if a0.end != a1.begin or a0.gpst != b0.gpst or a0.fingerprint != b0.fingerprint:
            continue
        if not equal_ranges(previous["path"], a0.begin, incoming["path"], b0.begin, b1.end - b0.begin):
            continue
        # The incoming head can start partway through an epoch. Require the
        # entire head to be an exact suffix of the retained previous bytes.
        prefix_size = b1.end - new[0].begin
        old_prefix_begin = a1.end - prefix_size
        if old_prefix_begin < previous["begin"]:
            continue
        if not equal_ranges(previous["path"], old_prefix_begin, incoming["path"], new[0].begin, prefix_size):
            continue
        # Replace the outgoing tail only when it is an exact prefix of the
        # incoming continuation, including incomplete asynchronous tail frames.
        tail_size = previous["end"] - a1.end
        if b1.end + tail_size > incoming["end"]:
            continue
        if not equal_ranges(previous["path"], a1.end, incoming["path"], b1.end, tail_size):
            continue
        return {
            "previous": previous["path"],
            "incoming": incoming["path"],
            "anchor_gpst": b1.gpst,
            "previous_cut": a1.end,
            "incoming_cut": b1.end,
            "incoming_duplicate": [new[0].begin, b1.end],
            "previous_duplicate": [a1.end, previous["end"]],
            "prefix_match": [old_prefix_begin, a1.end],
            "tail_match": [b1.end, b1.end + tail_size],
            "proof": "two consecutive EOE intervals plus full byte comparison of both discarded ranges",
        }
    raise ValueError(f"Unverified overlap: {previous['path']} -> {incoming['path']}")


def build_plan(sources):
    selected, joins = [], []
    for source in tqdm(sources, desc="Overlap proofs", unit="file"):
        if source.get("time_reversals") or source.get("time_conflicts"):
            raise ValueError(f"Conflicting source timestamps: {source['path']}")
        rows = useful_rows(source)
        if not rows or source["first_gpst"] is None:
            raise ValueError(f"No reliably timed frames: {source['path']}")
        current = {**source, "begin": rows[0].begin, "end": rows[-1].end}
        if selected and current["first_gpst"] <= selected[-1]["last_gpst"]:
            previous = selected[-1]
            if current["last_gpst"] <= previous["last_gpst"]:
                raise ValueError(f"Contained or nested overlap requires review: {current['path']}")
            join = join_sources(previous, current)
            previous["end"] = join["previous_cut"]
            current["begin"] = join["incoming_cut"]
            joins.append(join)
        selected.append(current)
    return {"schema": 2, "time_scale": "GPST", "sources": selected, "joins": joins}


def append_span(spans, path, begin, end):
    if spans and spans[-1]["source"] == path and spans[-1]["end"] == begin:
        spans[-1]["end"] = end
    else:
        spans.append({"source": path, "begin": begin, "end": end})


def segment_plan(plan):
    """Apply the NAV-per-second policy without dropping unassigned UBX bytes."""
    artifacts, events, names = [], [], set()
    segment = None
    group = []
    last_gpst = None
    pending_reason = "first_available"
    unassigned = {}

    def quarantine(path, epoch, reason):
        spans = unassigned.setdefault(path, [])
        append_span(spans, path, epoch.begin, epoch.end)
        events.append(
            {
                "type": reason,
                "source": path,
                "begin": epoch.begin,
                "end": epoch.end,
                "gpst": None if epoch.gpst == UNKNOWN else epoch.gpst,
            }
        )

    def flush():
        nonlocal segment, last_gpst, pending_reason
        if not group:
            return
        timestamp = group[0][1].gpst
        if not sum(e.nav for _, e in group):
            for path, e in group:
                quarantine(path, e, "no_nav_interval")
            segment = None
            pending_reason = "no_nav_interval"
            group.clear()
            return
        if any(e.flags & CONFLICT for _, e in group):
            raise ValueError(f"Conflicting GPST epoch at {gpst_label(timestamp)}")
        if last_gpst is not None and timestamp < last_gpst:
            raise ValueError("Reconstructed time runs backwards")
        reason = pending_reason
        if last_gpst is not None and timestamp > last_gpst + 1:
            reason = "missing_interval"
            events.append(
                {
                    "type": "missing_interval",
                    "after_gpst": last_gpst,
                    "next_gpst": timestamp,
                    "missing_seconds": timestamp - last_gpst - 1,
                }
            )
            segment = None
        if last_gpst is not None and timestamp // 86400 != last_gpst // 86400:
            reason = "gpst_midnight" if timestamp == last_gpst + 1 else reason
            segment = None
        if segment is None:
            name = gpst_label(timestamp) + ".ubx"
            if name in names:
                raise ValueError(f"Segment filename collision: {name}")
            names.add(name)
            segment = {
                "name": name,
                "kind": "gpst_segment",
                "start_gpst": timestamp,
                "end_gpst": timestamp,
                "reason": reason,
                "frames": 0,
                "nav_seconds": 0,
                "spans": [],
            }
            artifacts.append(segment)
        for path, e in group:
            append_span(segment["spans"], path, e.begin, e.end)
            segment["frames"] += e.frames
        segment["nav_seconds"] += 1
        segment["end_gpst"] = timestamp
        last_gpst = timestamp
        pending_reason = "continuation"
        group.clear()

    for source in tqdm(plan["sources"], desc="GPST segments", unit="file"):
        path = source["path"]
        for e in epochs(Path(source["index"])):
            if e.flags & NOISE:
                events.append({"type": "non_ubx_or_corrupt_bytes", "source": path, "begin": e.begin, "end": e.end})
                continue
            if e.begin < source["begin"] or e.end > source["end"]:
                continue
            if e.gpst == UNKNOWN:
                # Do not invent an epoch for untimed tails or damaged runs.
                quarantine(path, e, "unknown_time")
                continue
            if group and e.gpst != group[0][1].gpst:
                flush()
            group.append((path, e))
    flush()
    for path, spans in unassigned.items():
        artifacts.append({"name": "unassigned/" + Path(path).name, "kind": "unassigned_frames", "spans": spans})
    for artifact in artifacts:
        artifact["size"] = sum(s["end"] - s["begin"] for s in artifact["spans"])
    # Prove that each selected source byte is accounted for exactly once,
    # either in an output or as an explicitly recorded non-UBX/corrupt range.
    coverage = {s["path"]: [] for s in plan["sources"]}
    for artifact in artifacts:
        for span in artifact["spans"]:
            coverage[span["source"]].append((span["begin"], span["end"]))
    for event in events:
        if event["type"] == "non_ubx_or_corrupt_bytes":
            coverage[event["source"]].append((event["begin"], event["end"]))
    for source in plan["sources"]:
        cursor = source["begin"]
        for begin, end in sorted(coverage[source["path"]]):
            begin, end = max(begin, source["begin"]), min(end, source["end"])
            if begin >= end:
                continue
            if begin != cursor:
                raise ValueError(f"Unaccounted or multiply selected bytes: {source['path']} at {cursor}")
            cursor = end
        if cursor != source["end"]:
            raise ValueError(f"Unaccounted tail: {source['path']} at {cursor}")
    output_bytes = sum(a["size"] for a in artifacts)
    excluded = sum(s.get("excluded_bytes", 0) for s in plan["sources"])
    accounting = {
        "source_bytes": sum(s["size"] for s in plan["sources"]),
        "output_bytes": output_bytes,
        "non_ubx_or_corrupt_bytes": excluded,
    }
    accounting["verified_duplicate_bytes"] = accounting["source_bytes"] - output_bytes - excluded
    return {**plan, "artifacts": artifacts, "events": events, "byte_accounting": accounting}
