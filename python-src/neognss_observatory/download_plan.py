# SPDX-License-Identifier: GPL-3.0-only
"""Listing-based inventory; checksums never determine availability."""

import datetime as dt
import fnmatch
import hashlib
import math
import re
import tomllib
from collections import Counter, defaultdict
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit

from .download_common import (
    GPS_EPOCH,
    DownloadError,
    days,
    gps_week,
    local_path,
    now,
    safe_relative,
    tool_identity,
)
from .download_http import ARCHIVE, DEFAULT_NETWORK
from .download_plan_io import plan_digest, read_plan


class DirectoryLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.names = set()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "a" and attrs.get("title") == "DataDirectory":
            name = attrs.get("href", "").rstrip("/")
            if name.isdigit():
                self.names.add(int(name))


def parse_listing(text):
    if text is None:
        return {}
    if "<html" in text.lower() or "<!doctype" in text.lower():
        raise DownloadError("Expected a plain archive listing, received HTML")
    result, declared = {}, None
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"# Total number of files = (\d+)", line.strip())
        if match:
            declared = int(match[1])
        if line.startswith("#"):
            continue
        match = re.fullmatch(r"(\S+)\s+(\d+)\s*", line)
        if not match:
            raise DownloadError("Unrecognized archive listing row")
        name, size = match.groups()
        if "/" in name or name in result:
            raise DownloadError("Duplicate or non-basename archive listing entry")
        safe_relative(name)
        result[name] = int(size)
    # Empty CDDIS wildcard responses may have no footer.
    if result and declared is None or declared is not None and declared != len(result):
        raise DownloadError("Archive listing count mismatch or missing footer; inventory may be truncated")
    return result


def read_config(path):
    try:
        raw = Path(path).read_bytes()
        config = tomllib.loads(raw.decode())
        allowed = {"start", "end", "guard_days", "network", "products", "checksum", "external_inputs", "description"}
        if set(config) - allowed:
            raise ValueError("Unknown top-level configuration key")
        for key in ("start", "end"):
            if key in config:
                config[key] = dt.date.fromisoformat(str(config[key])).isoformat()
        if "start" not in config or dt.date.fromisoformat(config["start"]) < GPS_EPOCH:
            raise ValueError("start must be on or after 1980-01-06")
        if config.get("end", config["start"]) < config["start"]:
            raise ValueError("end precedes start")
        guard = config.setdefault("guard_days", 1)
        if type(guard) is not int or not 0 <= guard <= 7:
            raise ValueError("guard_days must be an integer between 0 and 7")
        network = config.setdefault("network", {})
        if set(network) - DEFAULT_NETWORK.keys():
            raise ValueError("Unknown network setting")
        config["network"] = {**DEFAULT_NETWORK, **network}
        for key, value in config["network"].items():
            if type(value) not in (int, float) or not math.isfinite(value) or value < (0 if key == "retries" else 0.001):
                raise ValueError(f"Invalid network setting: {key}")
        if type(config["network"]["workers"]) is not int or not 1 <= config["network"]["workers"] <= 16:
            raise ValueError("workers must be an integer between 1 and 16")
        if type(config["network"]["retries"]) is not int or config["network"]["retries"] > 20:
            raise ValueError("retries must be an integer between 0 and 20")
        products = config["products"]
        if not isinstance(products, list) or not products:
            raise ValueError("products must be a nonempty array of tables")
        names = set()
        for product in products:
            if set(product) - {"id", "layout", "base", "pattern", "required", "family", "url", "format"}:
                raise ValueError("Unknown product setting")
            name = product["id"]
            if not re.fullmatch(r"[a-z][a-z0-9_-]*", name) or name in names:
                raise ValueError("Product IDs must be unique lowercase identifiers")
            names.add(name)
            if product["layout"] not in ("week", "year", "day", "static"):
                raise ValueError("Unknown product layout")
            if product.get("format") not in ("sp3", "clk", "erp", "bias", "ionex", "rinex-nav", "antex"):
                raise ValueError("Each product needs a supported format")
            if type(product.setdefault("required", True)) is not bool:
                raise ValueError("required must be boolean")
            if product["layout"] == "static":
                if not product["url"].startswith("https://files.igs.org/pub/station/general/"):
                    raise ValueError("Static input must use the IGS station/general HTTPS source")
                safe_relative(urlsplit(product["url"]).path.lstrip("/"))
            else:
                safe_relative(product["base"])
                if not product["base"].startswith("gnss/"):
                    raise ValueError("Product base must be relative to archive/gnss/")
                product["pattern"].format(yyyy=2025, doy="096", yy="25", week=2361)
                if "/" in product["pattern"]:
                    raise ValueError("Product patterns must match basenames")
        checksum = config.get("checksum")
        if checksum:
            if set(checksum) - {"source_url", "local_file", "algorithm"}:
                raise ValueError("Unknown checksum setting")
            if checksum.get("algorithm", "sha512") not in ("sha512", "md5"):
                raise ValueError("Unsupported checksum algorithm")
            if not checksum["source_url"].startswith(ARCHIVE + "gnss/"):
                raise ValueError("Checksum source must be within CDDIS archive/gnss/")
            if not Path(checksum["local_file"]).expanduser().is_file():
                raise ValueError("Configured local checksum snapshot does not exist")
        for entry in config.get("external_inputs", []):
            if set(entry) - {"id", "required", "reason"} or not all(key in entry for key in ("id", "required", "reason")):
                raise ValueError("External inputs require id, required and reason")
        return config, hashlib.sha512(raw).hexdigest()
    except (KeyError, ValueError, TypeError, AttributeError, OSError) as exc:
        raise DownloadError(f"Invalid configuration: {exc}") from exc


def tokens(day):
    return {"yyyy": day.year, "yy": day.strftime("%y"), "doy": day.strftime("%j"), "week": gps_week(day)}


def directory(product, day):
    base = product["base"].rstrip("/") + "/"
    if product["layout"] == "week":
        return base + str(gps_week(day)) + "/"
    if product["layout"] == "year":
        return base + f"{day.year}/brdc/"
    return base + f"{day.year}/{day:%j}/"


class Inventory:
    def __init__(self, http, checkpoint=None):
        self.http = http
        self.checkpoint = checkpoint
        self.cache = {}
        self.evidence = {}

    def text(self, relative, listing=False):
        url = ARCHIVE + relative + ("*?list" if listing else "")
        text = self.http.text(url)
        self.evidence[url] = {"time": now(), "sha512": hashlib.sha512(text.encode()).hexdigest() if text is not None else None}
        return text

    def listing(self, relative):
        if relative not in self.cache:
            url = ARCHIVE + relative + "*?list"
            cached = self.checkpoint.get(url, "listing") if self.checkpoint else None
            if cached is not None:
                self.cache[relative], self.evidence[url] = cached
            else:
                self.cache[relative] = parse_listing(self.text(relative, True))
                if self.checkpoint:
                    self.checkpoint.put(url, "listing", self.cache[relative], self.evidence[url])
        return self.cache[relative]

    def directories(self, relative):
        url = ARCHIVE + relative
        cached = self.checkpoint.get(url, "directories") if self.checkpoint else None
        if cached is not None:
            directories, self.evidence[url] = cached
            return directories
        text = self.text(relative)
        if text is None:
            return []
        parser = DirectoryLinks()
        parser.feed(text)
        if not parser.names:
            raise DownloadError(f"No numeric archive directories found at {relative}")
        directories = sorted(parser.names)
        if self.checkpoint:
            self.checkpoint.put(url, "directories", directories, self.evidence[url])
        return directories

    def latest(self, product):
        base = product["base"].rstrip("/") + "/"
        numbers = self.directories(base)
        if not numbers:
            raise DownloadError(f"Cannot discover latest directory for {product['id']}")
        if product["layout"] == "week":
            return GPS_EPOCH + dt.timedelta(days=max(numbers) * 7 + 6)
        year = max(numbers)
        if product["layout"] == "day":
            doy = max(self.directories(f"{base}{year}/"))
            return dt.date(year, 1, 1) + dt.timedelta(days=doy - 1)
        listing = self.listing(f"{base}{year}/brdc/")
        matches = [
            day
            for day in days(dt.date(year, 1, 1), dt.date(year, 12, 31))
            if any(fnmatch.fnmatchcase(name, product["pattern"].format(**tokens(day))) for name in listing)
        ]
        if not matches:
            raise DownloadError(f"No matching dated products in latest year for {product['id']}")
        return max(matches)


def attach_checksums(plan, config, progress=None):
    """One sequential pass, bounded by the number of selected basenames."""
    if not config:
        return
    algorithm = config.get("algorithm", "sha512")
    scope = config["source_url"].rsplit("/", 1)[0] + "/"
    selected = defaultdict(list)
    for item in plan["files"]:
        if item["url"].startswith(scope):
            selected[Path(item["path"]).name].append(item)
    matches = {name: set() for name in selected}
    digest = hashlib.sha512()
    malformed, lines = 0, 0
    path = Path(config["local_file"]).expanduser()
    size = path.stat().st_size
    width = 128 if algorithm == "sha512" else 32
    with path.open("rb") as stream:
        position = 0
        for raw in stream:
            digest.update(raw)
            lines += 1
            position += len(raw)
            if lines % 100000 == 0 and progress:
                progress(position, size)
            if not raw.strip() or raw.startswith(b"#"):
                continue
            parts = raw.rstrip(b"\r\n").split(None, 1)
            if len(parts) != 2 or len(parts[0]) != width or re.fullmatch(b"[0-9a-fA-F]+", parts[0]) is None:
                malformed += 1
                continue
            name = parts[1].lstrip(b"*").decode("utf-8", errors="replace")
            if name in matches:
                # More than one digest is sufficient to establish ambiguity.
                if len(matches[name]) < 2:
                    matches[name].add(parts[0].decode().lower())
    plan["checksum_snapshot"] = {
        "source_url": config["source_url"],
        "sha512": digest.hexdigest(),
        "size": size,
        "algorithm": algorithm,
        "lines": lines,
        "malformed_lines": malformed,
    }
    for name, items in selected.items():
        ambiguous = len(matches[name]) > 1 or len(items) > 1
        for item in items:
            item["upstream_status"] = "ambiguous" if ambiguous else "available" if matches[name] else "unavailable"
            if item["upstream_status"] == "available":
                item["expected_digest"] = next(iter(matches[name]))
                item["digest_algorithm"] = algorithm
                item["checksum_snapshot"] = plan["checksum_snapshot"]
    if progress:
        progress(size, size)


def upstream_evidence_status(item, record):
    """Compare the current snapshot with recorded bytes, independently of completion."""
    if not item.get("expected_digest"):
        return item["upstream_status"]
    actual = record.get("sha512") if item["digest_algorithm"] == "sha512" else None
    if item["digest_algorithm"] == record.get("digest_algorithm") and actual is None:
        actual = record.get("upstream_actual_digest")
        # Legacy successful records did enforce the expected digest.
        if actual is None and record.get("upstream_status") == "verified":
            actual = record.get("expected_digest")
    if actual is None:
        return "available"
    return "verified" if actual == item["expected_digest"] else "mismatch"


def recorded_complete(item, record):
    if record.get("status") != "complete" or record.get("url") != item["url"]:
        return False
    if item["size"] is not None and record.get("size") != item["size"]:
        return False
    return not any(item.get(field) and item[field] != record.get(field) for field in ("etag", "last_modified"))


def reusable_candidate(item, record, root):
    if not recorded_complete(item, record):
        return False
    if root is None:
        return True
    try:
        path = local_path(root, item["path"])
        return path.is_file() and path.stat().st_size == record["size"]
    except OSError:
        return False


def make_plan(
    config,
    config_hash,
    http,
    report=lambda message: None,
    checksum_progress=None,
    *,
    records=None,
    root=None,
    inventory_progress=None,
    checkpoint=None,
):
    inventory = Inventory(http, checkpoint)
    start = dt.date.fromisoformat(config["start"])
    latest_directories = {}
    if "end" in config:
        end = dt.date.fromisoformat(config["end"])
    else:
        for product in config["products"]:
            if product["layout"] != "static":
                latest_directories[product["id"]] = inventory.latest(product).isoformat()
        if not latest_directories:
            raise DownloadError("An open-ended plan needs a dated product")
        end = dt.date.fromisoformat(max(latest_directories.values()))
    if end < start:
        raise DownloadError("No available range on or after start")
    guard = dt.timedelta(days=config["guard_days"])
    first, last = start - guard, end + guard
    weeks = sorted({gps_week(day) for day in days(first, last)})
    files, slots = {}, []
    records = records or {}
    seen, pending = set(), set()

    def inventory_update(completed, final=False):
        if final:
            seen.clear()
            pending.clear()
        for path in files.keys() - seen:
            if not reusable_candidate(files[path], records.get(path, {}), root):
                pending.add(path)
            seen.add(path)
        state = {
            "completed": completed,
            "total": len(weeks),
            "selected": len(files),
            "pending": len(pending),
            "reusable": len(files) - len(pending),
            "directories": len(inventory.cache),
            "final": final,
            "cached": checkpoint.hits if checkpoint else 0,
        }
        if inventory_progress:
            inventory_progress(state)
        else:
            report(
                f"Weeks inventoried: {completed}/{len(weeks)}; selected files: {len(files)}; need download: {len(pending)}; reusable: {state['reusable']}; directories: {len(inventory.cache)}"
            )

    inventory_update(0)
    for index, week in enumerate(weeks, 1):
        for day in days(
            max(first, GPS_EPOCH + dt.timedelta(days=week * 7)), min(last, GPS_EPOCH + dt.timedelta(days=week * 7 + 6))
        ):
            for product in config["products"]:
                if product["layout"] == "static":
                    continue
                relative = directory(product, day)
                pattern = product["pattern"].format(**tokens(day))
                listing = inventory.listing(relative)
                names = sorted(name for name in listing if fnmatch.fnmatchcase(name, pattern))
                slot = {
                    "product": product["id"],
                    "day": day.isoformat(),
                    "week": week,
                    "required": product["required"],
                    "paths": [],
                }
                if len(names) != 1:
                    slot["issue"] = "missing" if not names else "ambiguous-selection"
                    slot["candidates"] = names
                else:
                    name = names[0]
                    path = "cddis/archive/" + relative + name
                    if path in files and files[path]["product"] != product["id"]:
                        raise DownloadError("Two product selectors resolve to the same destination")
                    slot["paths"] = [path]
                    files.setdefault(
                        path,
                        {
                            "path": path,
                            "url": ARCHIVE + relative + name,
                            "size": listing[name],
                            "product": product["id"],
                            "family": product.get("family"),
                            "format": product["format"],
                            "required": product["required"],
                            "nominal_dates": [],
                            "coverage_status": "unverified",
                            "upstream_status": "unavailable",
                        },
                    )["nominal_dates"].append(day.isoformat())
                slots.append(slot)
        inventory_update(index)
    # Freeze the discovered frontier at actual selected filename dates, not the last directory's Saturday.
    if "end" not in config:
        observed = [date for item in files.values() for date in item["nominal_dates"]]
        if not observed:
            raise DownloadError("No selected products found in the discovered range")
        end = dt.date.fromisoformat(max(observed))
        last = end + guard
        slots = [slot for slot in slots if slot["day"] <= last.isoformat()]
    for product in config["products"]:
        if product["layout"] != "static":
            continue
        slot = {"product": product["id"], "day": None, "week": None, "required": product["required"], "paths": []}
        with http.request(product["url"]) as response:
            if response.status_code == 404:
                slot["issue"] = "missing"
            elif response.status_code != 200:
                raise DownloadError(f"Static input returned HTTP {response.status_code}")
            else:
                path = "igs/" + urlsplit(product["url"]).path.lstrip("/")
                slot["paths"] = [path]
                files[path] = {
                    "path": path,
                    "url": product["url"],
                    "size": int(response.headers["Content-Length"]) if "Content-Length" in response.headers else None,
                    "product": product["id"],
                    "family": product.get("family"),
                    "format": product["format"],
                    "required": product["required"],
                    "nominal_dates": [],
                    "coverage_status": "unverified",
                    "upstream_status": "unavailable",
                    "etag": response.headers.get("ETag"),
                    "last_modified": response.headers.get("Last-Modified"),
                }
        slots.append(slot)
    for entry in config.get("external_inputs", []):
        slots.append({"product": entry["id"], "day": None, "week": None, "paths": [], "issue": "external-input", **entry})
    plan = {
        "schema": 2,
        "created_at": now(),
        "start": start.isoformat(),
        "end": end.isoformat(),
        "guard_days": config["guard_days"],
        "latest_directory_bounds": latest_directories,
        "config_sha512": config_hash,
        "tool": tool_identity(),
        "network": config["network"],
        "products": config["products"],
        "files": sorted(files.values(), key=lambda item: item["path"]),
        "slots": slots,
        "inventory": inventory.evidence,
        "latest_selected_dates": {
            product["id"]: max(
                (date for item in files.values() if item["product"] == product["id"] for date in item["nominal_dates"]),
                default=None,
            )
            for product in config["products"]
        },
    }
    attach_checksums(plan, config.get("checksum"), checksum_progress)
    inventory_update(len(weeks), final=True)
    plan["sha512"] = plan_digest(plan)
    return plan


def load_plan(path):
    try:
        plan = read_plan(path)
        paths = set()
        for item in plan["files"]:
            safe_relative(item["path"])
            expected_url = ARCHIVE + item["path"].removeprefix("cddis/archive/")
            if item["path"].startswith("igs/pub/station/general/"):
                expected_url = "https://files.igs.org/" + item["path"].removeprefix("igs/")
            elif not item["path"].startswith("cddis/archive/gnss/"):
                raise ValueError("Unexpected product path")
            if item["url"] != expected_url or item["path"] in paths:
                raise ValueError("Invalid URL/path mapping or duplicate plan entry")
            paths.add(item["path"])
        return plan
    except (KeyError, ValueError, TypeError, AttributeError, OSError) as exc:
        raise DownloadError(f"Cannot read plan: {exc}") from exc


def summarize(plan, records=None):
    records = records or {}
    complete = set()
    for item in plan["files"]:
        record = records.get(item["path"], {})
        if recorded_complete(item, record):
            complete.add(item["path"])
    weeks = defaultdict(list)
    issues = Counter()
    for slot in plan["slots"]:
        if "issue" in slot:
            issues[slot["issue"]] += 1
        if slot["week"] is not None:
            weeks[slot["week"]].append(slot)

    required_static = [slot for slot in plan["slots"] if slot["required"] and slot["day"] is None]
    static_available = all(not slot.get("issue") for slot in required_static)
    static_complete = static_available and all(all(path in complete for path in slot["paths"]) for slot in required_static)
    per_day = defaultdict(list)
    for slot in plan["slots"]:
        if slot["day"] and slot["required"]:
            per_day[slot["day"]].append(slot)

    def through(downloaded):
        if not (static_complete if downloaded else static_available):
            return None
        result = None
        for day in days(dt.date.fromisoformat(plan["start"]), dt.date.fromisoformat(plan["end"])):
            # Include guard inputs for every target processing day.
            neighbors = days(day - dt.timedelta(days=plan.get("guard_days", 0)), day + dt.timedelta(days=plan.get("guard_days", 0)))
            slots = [slot for neighbor in neighbors for slot in per_day.get(neighbor.isoformat(), [])]
            if any(slot.get("issue") or downloaded and any(path not in complete for path in slot["paths"]) for slot in slots):
                break
            result = day.isoformat()
        return result

    return {
        "start": plan["start"],
        "end": plan["end"],
        "weeks": len(weeks),
        "weeks_complete": sum(
            all(not s.get("issue") and all(p in complete for p in s["paths"]) for s in slots if s["required"])
            for slots in weeks.values()
        ),
        "weeks_needing_downloads": sum(any(any(p not in complete for p in s["paths"]) for s in slots) for slots in weeks.values()),
        "weeks_with_missing_inputs": sum(any(s.get("issue") and s["required"] for s in slots) for slots in weeks.values()),
        "files": len(plan["files"]),
        "files_complete": sum(item["path"] in complete for item in plan["files"]),
        "known_bytes": sum(item["size"] or 0 for item in plan["files"]),
        "known_bytes_pending": sum(item["size"] or 0 for item in plan["files"] if item["path"] not in complete),
        "unknown_size_files": sum(item["size"] is None for item in plan["files"]),
        "issues": dict(issues),
        "upstream_checksum": dict(
            Counter(
                upstream_evidence_status(item, records[item["path"]]) if item["path"] in complete else item["upstream_status"]
                for item in plan["files"]
            )
        ),
        "latest_selected_dates": plan["latest_selected_dates"],
        "required_inputs_available_through": through(False),
        "required_inputs_downloaded_through": through(True),
        "coverage_status": "unverified",
    }
