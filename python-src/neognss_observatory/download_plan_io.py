# SPDX-License-Identifier: GPL-3.0-only
"""Atomic, line-oriented plan serialization with a verified completion footer."""

import hashlib
import json
import os
import tempfile
from collections import Counter
from pathlib import Path

from .download_common import json_digest


def plan_rows(plan):
    metadata = {key: value for key, value in plan.items() if key not in ("files", "slots", "inventory", "sha512")}
    metadata["schema"] = 2
    yield {"type": "header", "data": metadata}
    for item in plan.get("files", []):
        yield {"type": "file", "data": item}
    for slot in plan.get("slots", []):
        yield {"type": "slot", "data": slot}
    for url, evidence in sorted(plan.get("inventory", {}).items()):
        yield {"type": "inventory", "url": url, "data": evidence}


def encode_row(row):
    return (json.dumps(row, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def plan_digest(plan):
    digest = hashlib.sha512()
    for row in plan_rows(plan):
        digest.update(encode_row(row))
    return digest.hexdigest()


def write_plan(path, plan):
    """Never assemble a full JSON document/string; replace only a complete plan."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        digest, counts = hashlib.sha512(), Counter()
        with os.fdopen(fd, "wb") as stream:
            for row in plan_rows(plan):
                raw = encode_row(row)
                digest.update(raw)
                counts[row["type"]] += 1
                stream.write(raw)
            stream.write(encode_row({"type": "footer", "sha512": digest.hexdigest(), "counts": dict(counts)}))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def read_plan(path):
    with Path(path).open("rb") as stream:
        first = stream.readline()
        # Read-only compatibility for plans made before JSON Lines was introduced.
        if first.strip() == b"{" or b'"type"' not in first:
            plan = json.loads(first + stream.read())
            expected = plan.pop("sha512")
            if plan["schema"] != 1 or json_digest(plan) != expected:
                raise ValueError("Legacy plan hash or schema mismatch")
            plan["sha512"] = expected
            return plan
        header = json.loads(first)
        if header.get("type") != "header" or header["data"].get("schema") != 2:
            raise ValueError("Expected a schema-2 JSON Lines header")
        plan = header["data"]
        if any(key in plan for key in ("files", "slots", "inventory", "sha512")):
            raise ValueError("Collections cannot be embedded in a JSON Lines header")
        plan.update(files=[], slots=[], inventory={})
        digest, counts = hashlib.sha512(first), Counter(header=1)
        for raw in stream:
            row = json.loads(raw)
            kind = row["type"]
            if kind == "footer":
                if row["sha512"] != digest.hexdigest() or row["counts"] != dict(counts):
                    raise ValueError("Plan hash or record count mismatch")
                if stream.read(1):
                    raise ValueError("Unexpected content after plan footer")
                plan["sha512"] = row["sha512"]
                return plan
            if kind == "file":
                plan["files"].append(row["data"])
            elif kind == "slot":
                plan["slots"].append(row["data"])
            elif kind == "inventory":
                if row["url"] in plan["inventory"]:
                    raise ValueError("Duplicate inventory URL")
                plan["inventory"][row["url"]] = row["data"]
            else:
                raise ValueError("Unknown or repeated plan record type")
            digest.update(raw)
            counts[kind] += 1
        raise ValueError("Incomplete plan: missing footer")
