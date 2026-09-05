# SPDX-License-Identifier: GPL-3.0-only
"""Durable checkpoints for an unfinished inventory, separate from download state."""

import fcntl
import json
import sqlite3
from pathlib import Path

from .download_common import DownloadError


class InventoryCache:
    def __init__(self, path, identity, refresh=False):
        self.path = Path(path)
        self.identity = identity
        self.refresh = refresh
        self.connection = None
        self.lock = None
        self.hits = 0

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = self.path.with_name(self.path.name + ".lock").open("a")
        try:
            try:
                fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise DownloadError("Another planner is using this inventory checkpoint") from exc
            self.connection = sqlite3.connect(self.path)
            self.connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS pages (
                    url TEXT NOT NULL, kind TEXT NOT NULL, payload TEXT NOT NULL,
                    evidence TEXT NOT NULL, PRIMARY KEY (url, kind)
                );
                """
            )
            metadata = dict(self.connection.execute("SELECT key, value FROM metadata"))
            if self.refresh or metadata.get("identity") != self.identity or metadata.get("state") == "complete":
                with self.connection:
                    self.connection.execute("DELETE FROM pages")
                    self.connection.execute("DELETE FROM metadata")
                    self.connection.executemany(
                        "INSERT INTO metadata VALUES (?, ?)", [("identity", self.identity), ("state", "incomplete")]
                    )
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def get(self, url, kind):
        row = self.connection.execute("SELECT payload, evidence FROM pages WHERE url = ? AND kind = ?", (url, kind)).fetchone()
        if row is None:
            return None
        self.hits += 1
        return json.loads(row[0]), json.loads(row[1])

    def put(self, url, kind, payload, evidence):
        # Commit each validated response so later network/parser failures lose no prior pages.
        with self.connection:
            self.connection.execute(
                "INSERT OR REPLACE INTO pages VALUES (?, ?, ?, ?)",
                (url, kind, json.dumps(payload, separators=(",", ":")), json.dumps(evidence)),
            )

    def complete(self):
        with self.connection:
            self.connection.execute("INSERT OR REPLACE INTO metadata VALUES ('state', 'complete')")

    def __exit__(self, *_):
        if self.connection is not None:
            self.connection.close()
            self.connection = None
        if self.lock is not None:
            self.lock.close()
            self.lock = None
