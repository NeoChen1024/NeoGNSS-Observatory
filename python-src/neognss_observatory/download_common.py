# SPDX-License-Identifier: GPL-3.0-only
"""Storage and identity helpers for reproducible product downloads."""

import contextlib
import datetime as dt
import fcntl
import hashlib
import importlib.metadata
import json
import os
import platform
import sqlite3
import subprocess
import tempfile
from pathlib import Path, PurePosixPath

GPS_EPOCH = dt.date(1980, 1, 6)


class DownloadError(Exception):
    """A safe-to-display operational error (never include credentials)."""


class AuthError(DownloadError):
    pass


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def digest_file(path, algorithm="sha512"):
    digest = hashlib.new(algorithm)
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def json_digest(value):
    return hashlib.sha512(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def safe_relative(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or "\\" in value or any(c in value for c in "?#%\x00\r\n"):
        raise DownloadError("Unsafe archive-relative path")
    return path


def local_path(root, relative):
    root = Path(root).resolve()
    path = root.joinpath(safe_relative(relative))
    if not path.resolve().is_relative_to(root):
        raise DownloadError("A product path escapes the download root")
    return path


def gps_week(day):
    # Calendar bucketing only, NOT a conversion of UTC observation epochs to GPST.
    return (day - GPS_EPOCH).days // 7


def days(start, end):
    for offset in range((end - start).days + 1):
        yield start + dt.timedelta(days=offset)


def tool_identity():
    identity = {"python": platform.python_version(), "dependencies": {}}
    for package in (
        "neognss-observatory",
        "requests",
        "urllib3",
        "certifi",
        "charset-normalizer",
        "idna",
        "click",
        "rich",
        "markdown-it-py",
        "mdurl",
        "pygments",
        "tqdm",
    ):
        try:
            identity["dependencies"][package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            identity["dependencies"][package] = "not-installed"
    identity["source_sha512"] = {path.name: digest_file(path) for path in sorted(Path(__file__).parent.glob("*.py"))}
    try:
        identity["git_commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=Path(__file__).parent, stderr=subprocess.DEVNULL, text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        identity["git_commit"] = None
    return identity


@contextlib.contextmanager
def root_lock(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    with open(local_path(root, ".download.lock"), "a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise DownloadError("Another writer is using this download root") from exc
        yield


def database(root):
    connection = sqlite3.connect(local_path(root, "manifest.sqlite"))
    connection.execute("PRAGMA journal_mode=WAL")
    connection.executescript(
        """
        CREATE TABLE IF NOT EXISTS files (
            path TEXT PRIMARY KEY, status TEXT NOT NULL, record TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY, time TEXT NOT NULL, path TEXT NOT NULL, record TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS plans (
            hash TEXT PRIMARY KEY, record TEXT NOT NULL
        );
        """
    )
    return connection


def save_record(connection, record):
    serialized = json.dumps(record, sort_keys=True)
    with connection:
        connection.execute("INSERT OR REPLACE INTO files VALUES (?, ?, ?)", (record["path"], record["status"], serialized))
        connection.execute("INSERT INTO events(time, path, record) VALUES (?, ?, ?)", (now(), record["path"], serialized))
