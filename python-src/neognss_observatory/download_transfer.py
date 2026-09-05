# SPDX-License-Identifier: GPL-3.0-only
"""Resumable transfers, payload integrity checks and offline manifest queries."""

import concurrent.futures
import contextlib
import gzip
import json
import os
import re
import sqlite3
import subprocess
import uuid
import zlib
from pathlib import Path

import requests

from .download_common import (
    AuthError,
    DownloadError,
    atomic_json,
    database,
    digest_file,
    local_path,
    now,
    root_lock,
    save_record,
    tool_identity,
)
from .download_plan import summarize


def quarantine(path):
    if path.exists():
        destination = path.with_name(path.name + ".quarantine-" + uuid.uuid4().hex)
        os.replace(path, destination)
        return destination.name
    return None


def validate(path, item, previous_hash=None):
    size = path.stat().st_size
    if not size or item["size"] is not None and size != item["size"]:
        raise DownloadError("File size differs from the frozen inventory")
    sha512 = digest_file(path)
    if previous_hash and sha512 != previous_hash:
        raise DownloadError("File differs from its recorded local SHA-512")
    if "expected_digest" in item:
        actual = sha512 if item["digest_algorithm"] == "sha512" else digest_file(path, "md5")
        if actual != item["expected_digest"]:
            raise DownloadError("Upstream checksum mismatch (download damage or stale upstream snapshot)")
    name = item["path"]
    try:
        if name.endswith(".gz"):
            with gzip.open(path, "rb") as stream:
                prefix = stream.read(8192)
                for _ in iter(lambda: stream.read(1024 * 1024), b""):
                    pass
        elif name.endswith(".Z"):
            # gzip supports Unix compress input. No decompressed product is retained.
            with subprocess.Popen(["gzip", "-cd", "--", str(path)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as process:
                prefix = process.stdout.read(8192)
                for _ in iter(lambda: process.stdout.read(1024 * 1024), b""):
                    pass
                if process.wait() != 0:
                    raise DownloadError("Unix compress integrity check failed")
        else:
            with path.open("rb") as stream:
                prefix = stream.read(8192)
    except (OSError, EOFError, zlib.error) as exc:
        raise DownloadError("Compressed product integrity check failed") from exc
    signatures = {
        "sp3": lambda p: p.startswith(b"#"),
        "clk": lambda p: b"RINEX VERSION / TYPE" in p,
        "rinex-nav": lambda p: b"RINEX VERSION / TYPE" in p,
        "ionex": lambda p: b"IONEX VERSION / TYPE" in p,
        "bias": lambda p: p.startswith((b"%=BIA", b"%=BSX")),
        "antex": lambda p: b"ANTEX VERSION / SYST" in p,
        "erp": lambda p: b"version" in p.lower() or b"mjd" in p.lower(),
    }
    if b"<html" in prefix.lower() or b"<!doctype html" in prefix.lower() or not signatures[item["format"]](prefix):
        raise DownloadError("Product header does not match the configured format")
    return {"size": size, "sha512": sha512, "upstream_status": "verified" if "expected_digest" in item else item["upstream_status"]}


def validator(headers):
    etag = headers.get("ETag")
    return etag if etag and not etag.startswith("W/") else headers.get("Last-Modified")


def download_one(root, item, http, old=None, update=lambda size, total: None):
    target = local_path(root, item["path"])
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = local_path(root, item["path"] + ".part")
    metadata_path = local_path(root, item["path"] + ".part.json")
    record = {**item, "status": "failed", "started_at": now(), "attempts": [], "quarantined": []}
    if target.exists():
        try:
            if old and any(item.get(field) and item[field] != old.get(field) for field in ("etag", "last_modified")):
                raise DownloadError("Static input identity differs from the new plan")
            # Preserve local history across replanning, but always check the new expected checksum too.
            checked = validate(target, item, old.get("sha512") if old and old.get("status") == "complete" else None)
            update(checked["size"], checked["size"])
            return {
                **record,
                **checked,
                "status": "complete",
                "action": "reused",
                "finished_at": now(),
                "etag": old.get("etag") if old else item.get("etag"),
                "last_modified": old.get("last_modified") if old else item.get("last_modified"),
            }
        except DownloadError:
            record["quarantined"].append(quarantine(target))
    for attempt in range(http.settings["retries"] + 1):
        if http.stopped.is_set():
            raise DownloadError("Download interrupted")
        metadata = {}
        if partial.exists() and metadata_path.exists():
            try:
                metadata = json.loads(metadata_path.read_text())
            except (ValueError, OSError):
                pass
        offset = partial.stat().st_size if partial.exists() else 0
        compatible = metadata.get("url") == item["url"] and metadata.get("size") == item["size"]
        compatible = compatible and metadata.get("expected_digest") == item.get("expected_digest")
        if offset and (not compatible or not metadata.get("validator") or item["size"] is not None and offset > item["size"]):
            record["quarantined"].append(quarantine(partial))
            metadata_path.unlink(missing_ok=True)
            offset = 0
        headers = {"Range": f"bytes={offset}-", "If-Range": metadata["validator"]} if offset else {}
        record["attempts"].append({"time": now(), "offset": offset})
        try:
            with http.request(item["url"], headers) as response:
                code = response.status_code
                record["attempts"][-1]["http_status"] = code
                if code == 404:
                    record["error"] = "File no longer available (HTTP 404)"
                    break
                if code == 416:
                    # A complete .part can exist after a crash before rename.
                    if offset and item["size"] == offset and metadata.get("validator") == validator(response.headers):
                        checked = validate(partial, item)
                        os.replace(partial, target)
                        metadata_path.unlink(missing_ok=True)
                        return {**record, **checked, "status": "complete", "finished_at": now()}
                    record["quarantined"].append(quarantine(partial))
                    metadata_path.unlink(missing_ok=True)
                    continue
                if code not in (200, 206):
                    raise DownloadError(f"Unexpected HTTP status {code}")
                if response.headers.get("Content-Encoding", "identity") != "identity":
                    raise DownloadError("Unexpected HTTP content encoding; byte ranges would be unsafe")
                if "text/html" in response.headers.get("Content-Type", "").lower():
                    http.stopped.set()
                    raise AuthError("Received an HTML page instead of a product; check authentication")
                total = item["size"]
                if code == 206:
                    match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", response.headers.get("Content-Range", ""))
                    if not offset or not match or int(match[1]) != offset or int(match[2]) != int(match[3]) - 1:
                        raise DownloadError("Invalid Content-Range response")
                    if validator(response.headers) != metadata["validator"]:
                        raise DownloadError("Remote file validator changed during resume")
                    remote_size = int(match[3])
                    if "Content-Length" in response.headers and int(response.headers["Content-Length"]) != remote_size - offset:
                        raise DownloadError("Range response Content-Length mismatch")
                else:
                    if offset:
                        # Server ignored Range or If-Range detected a new representation.
                        record["quarantined"].append(quarantine(partial))
                    offset = 0
                    remote_size = int(response.headers["Content-Length"]) if "Content-Length" in response.headers else None
                if total is not None and remote_size is not None and total != remote_size:
                    raise DownloadError("Remote file size changed since planning; generate a new plan")
                for field, header in (("etag", "ETag"), ("last_modified", "Last-Modified")):
                    if item.get(field) and item[field] != response.headers.get(header):
                        raise DownloadError("Static input changed since planning; generate a new plan")
                record["etag"] = response.headers.get("ETag")
                record["last_modified"] = response.headers.get("Last-Modified")
                metadata = {
                    "url": item["url"],
                    "size": total,
                    "expected_digest": item.get("expected_digest"),
                    "validator": validator(response.headers),
                }
                atomic_json(metadata_path, metadata)
                update(offset, total or remote_size)
                with partial.open("ab" if offset else "wb") as stream:
                    for block in response.iter_content(256 * 1024):
                        if http.stopped.is_set():
                            raise DownloadError("Download interrupted")
                        if offset == 0 and (b"<html" in block[:8192].lower() or b"<!doctype html" in block[:8192].lower()):
                            http.stopped.set()
                            raise AuthError("Received an HTML login/error page instead of a product")
                        stream.write(block)
                        offset += len(block)
                        if total is not None and offset > total:
                            raise DownloadError("Response exceeds the planned file size")
                        update(offset, total or remote_size)
                    stream.flush()
                    os.fsync(stream.fileno())
                checked = validate(partial, item)
                os.replace(partial, target)
                metadata_path.unlink(missing_ok=True)
                return {**record, **checked, "status": "complete", "action": "downloaded", "finished_at": now()}
        except AuthError:
            raise
        except requests.RequestException:
            record["attempts"][-1]["error"] = "Interrupted HTTP body"
            if attempt < http.settings["retries"]:
                http.backoff(None, attempt)
                continue
            record["error"] = "Body transfer retry limit exhausted"
        except DownloadError as exc:
            record["error"] = str(exc)
            if partial.exists() and not http.stopped.is_set():
                record["quarantined"].append(quarantine(partial))
                metadata_path.unlink(missing_ok=True)
            break
    return {**record, "status": "failed", "finished_at": now(), "error": record.get("error", "Retry limit exhausted")}


def read_records(root):
    path = local_path(root, "manifest.sqlite")
    if not path.exists():
        return {}, []
    connection = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
    try:
        records = {path: json.loads(record) for path, record in connection.execute("SELECT path, record FROM files")}
        plans = [json.loads(record) for (record,) in connection.execute("SELECT record FROM plans ORDER BY rowid")]
        return records, plans
    finally:
        connection.close()


def fetch(plan, root, http, progress):
    with root_lock(root), contextlib.closing(database(root)) as connection:
        with connection:
            connection.execute("INSERT OR REPLACE INTO plans VALUES (?, ?)", (plan["sha512"], json.dumps(plan)))
        old, _ = read_records(root)
        identity = tool_identity()
        files = iter(plan["files"])
        pending = {}
        total_task = progress.add_task("Files", total=len(plan["files"]))
        auth_error = None
        with concurrent.futures.ThreadPoolExecutor(max_workers=http.settings["workers"]) as pool:

            def submit():
                item = next(files, None)
                if item is None:
                    return False
                task = progress.add_task(Path(item["path"]).name, total=item["size"])
                callback = lambda done, total: progress.update(task, completed=done, total=total)
                future = pool.submit(download_one, root, item, http, old.get(item["path"]), callback)
                pending[future] = (item, task)
                return True

            for _ in range(http.settings["workers"]):
                submit()
            try:
                while pending:
                    done, _ = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
                    for future in done:
                        item, task = pending.pop(future)
                        try:
                            record = future.result()
                        except AuthError as exc:
                            auth_error = exc
                            http.stopped.set()
                            record = {**item, "status": "failed", "error": str(exc), "finished_at": now()}
                        except (DownloadError, OSError) as exc:
                            record = {**item, "status": "failed", "error": str(exc), "finished_at": now()}
                        record.update(plan_sha512=plan["sha512"], tool=identity)
                        save_record(connection, record)
                        progress.remove_task(task)
                        progress.advance(total_task)
                        progress.console.print(
                            f"{record['status']}: {item['path']}" + (f" ({record['error']})" if "error" in record else ""),
                            markup=False,
                            soft_wrap=True,
                        )
                        if not http.stopped.is_set():
                            submit()
            except BaseException:
                http.stopped.set()
                raise
        if auth_error:
            raise auth_error
    records, _ = read_records(root)
    failed = any(item["required"] and records.get(item["path"], {}).get("status") != "complete" for item in plan["files"])
    failed = failed or any(slot["required"] and slot.get("issue") for slot in plan["slots"])
    return summarize(plan, records), int(bool(failed))


def verify(root, report):
    failures = 0
    with root_lock(root), contextlib.closing(database(root)) as connection:
        records, _ = read_records(root)
        for path, record in records.items():
            if record["status"] not in ("complete", "corrupt"):
                continue
            try:
                checked = validate(local_path(root, path), record, record.get("sha512"))
                record.update(checked, status="complete", verified_at=now())
            except (DownloadError, OSError) as exc:
                failures += 1
                record.update(status="corrupt", error=str(exc), verified_at=now())
            save_record(connection, record)
            report(f"{record['status']}: {path}")
    return failures
