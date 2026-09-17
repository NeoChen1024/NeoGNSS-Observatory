#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Mirror closed mosaic daily SBF files, compress locally, and publish via FTPS."""

import contextlib
import copy
import ftplib
import hashlib
import io
import json
import logging
import lzma
import math
import os
import queue
import re
import selectors
import shutil
import signal
import socket
import ssl
import subprocess
import threading
import time
from datetime import date, datetime, timedelta, timezone
from pathlib import Path, PurePosixPath

import click
from rich.console import Console
from rich.logging import RichHandler

from .download_common import (
    DownloadError,
    atomic_json,
    json_digest,
    local_path,
    root_lock,
)

LOG = logging.getLogger("neognss_observatory.mosaic_push")


class PushError(Exception):
    """An actionable configuration, storage, or transfer failure."""


class InvalidPartial(PushError):
    """A partial cannot be safely used for another transfer attempt."""


class StorageBlocked(PushError):
    """Admission must wait for space without delaying completed upload retries."""


def object_fields(value, allowed, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or set(required) - set(value):
        raise PushError(f"Expected an object with fields {', '.join(allowed)}; required: {', '.join(required)}")


def text_field(value, label, empty=False):
    if not isinstance(value, str) or (not value and not empty) or any(c in value for c in "\r\n\x00"):
        raise PushError(f"Invalid {label}")
    return value


def integer(value, label, low, high):
    if type(value) is not int or not low <= value <= high:
        raise PushError(f"{label} must be an integer in {low}..{high}")
    return value


def remote_path(value):
    text_field(value, "remote path")
    if ".." in PurePosixPath(value).parts or "\\" in value:
        raise PushError("Remote paths must not contain '..' or backslashes")
    return value


def expand_path(template, day):
    """Use setFTPPushSBF substitutions, not unrestricted strftime."""
    substitutions = {
        "Y": f"{day.year:04d}",
        "y": f"{day.year % 100:02d}",
        "m": f"{day.month:02d}",
        "d": f"{day.day:02d}",
        "j": f"{(day - date(day.year, 1, 1)).days + 1:03d}",
        "%": "%",
    }
    result = []
    index = 0
    while index < len(template):
        char = template[index]
        if char == "%":
            index += 1
            if index == len(template) or template[index] not in substitutions:
                raise PushError("Destination path supports only %Y, %y, %m, %d, %j and %%")
            char = substitutions[template[index]]
        result.append(char)
        index += 1
    return remote_path("".join(result))


def read_config(path):
    try:

        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise PushError("Duplicate JSON configuration field")
                result[key] = value
            return result

        config = json.loads(path.read_text(), object_pairs_hook=unique_object)
    except (OSError, ValueError) as exc:
        raise PushError("Cannot read a valid JSON configuration") from exc
    if isinstance(config, dict) and "destination" in config:
        raise PushError('Replace the single "destination" with "destinations": {"primary": {...}}')
    object_fields(
        config,
        ("archive_root", "source", "destinations", "schedule", "network", "compression", "output", "storage"),
        ("archive_root", "source"),
    )
    storage = config.setdefault("storage", {})
    object_fields(storage, ("limit_gib",))
    limit = storage.setdefault("limit_gib", None)
    if limit is not None and (type(limit) not in (int, float) or not math.isfinite(limit) or not 8 <= limit <= 1048576):
        raise PushError("storage.limit_gib must be null or a finite number from 8 to 1048576 GiB")
    output = config.setdefault("output", {})
    object_fields(output, ("group", "mode", "dirmode"))
    group = output.setdefault("group", None)
    output.setdefault("mode", None)
    output.setdefault("dirmode", None)
    if any(value is not None for value in output.values()):
        if os.name != "posix":
            raise PushError("Output permissions require a POSIX platform")
    if group is not None:
        import grp

        try:
            if isinstance(group, str) and group:
                output["group"] = grp.getgrnam(group).gr_gid
            elif type(group) is int and group >= 0:
                output["group"] = grp.getgrgid(group).gr_gid
            else:
                raise ValueError
        except (KeyError, ValueError, OverflowError) as exc:
            raise PushError("output.group must name an existing group or numeric GID") from exc
    for field in ("mode", "dirmode"):
        mode = output[field]
        if mode is not None:
            if not isinstance(mode, str) or not re.fullmatch(r"0?[0-7]{3}", mode):
                raise PushError(f'output.{field} must be a three- or four-digit octal string, such as "0755"')
            output[field] = int(mode, 8)
    try:
        root = Path(text_field(config["archive_root"], "archive_root")).expanduser()
    except RuntimeError as exc:
        raise PushError("Cannot resolve archive_root home directory on this host") from exc
    if not root.is_absolute():
        raise PushError("archive_root must be absolute")
    config["archive_root"] = root.resolve()
    destinations = config.setdefault("destinations", {})
    if not isinstance(destinations, dict):
        raise PushError("destinations must be an object keyed by stable target names")
    for name in destinations:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", name):
            raise PushError(
                "Destination names must be 1..64 ASCII letters, digits, underscores, dots or hyphens, starting with a letter or digit"
            )
    endpoints = [("source", config["source"], True)] + [
        (f"destinations.{name}", endpoint, False) for name, endpoint in destinations.items()
    ]
    for label, endpoint, is_source in endpoints:
        fields = ("host", "port", "username", "password", "path")
        extra = ("station", "year_base") if is_source else ("enabled", "tls", "ca_file", "verify_tls")
        object_fields(endpoint, fields + extra)
        if not is_source:
            endpoint.setdefault("enabled", True)
            if type(endpoint["enabled"]) is not bool:
                raise PushError(f"{label}.enabled must be a boolean")
            if not endpoint["enabled"]:
                continue
            object_fields(endpoint, fields + extra, fields)
        else:
            object_fields(endpoint, fields + extra, ("host", "path"))
            endpoint.setdefault("port", 21)
            endpoint.setdefault("username", "anonymous")
            endpoint.setdefault("password", "")
        for field in ("host", "username", "password"):
            text_field(endpoint[field], f"{label}.{field}", empty=field == "password")
        integer(endpoint["port"], f"{label}.port", 1, 65535)
        remote_path(endpoint["path"])
        if not is_source:
            endpoint.setdefault("verify_tls", True)
            if type(endpoint["verify_tls"]) is not bool:
                raise PushError(f"{label}.verify_tls must be a boolean")
            endpoint.setdefault("tls", "explicit")
            if endpoint["tls"] not in ("explicit", "implicit"):
                raise PushError(f"{label}.tls must be explicit or implicit")
            if endpoint["verify_tls"] and endpoint.get("ca_file") is not None:
                ca = Path(text_field(endpoint["ca_file"], f"{label}.ca_file")).expanduser()
                if not ca.is_absolute() or not ca.is_file():
                    raise PushError(f"{label}.ca_file must be an existing absolute file path")
                endpoint["ca_file"] = str(ca)
            expand_path(endpoint["path"], date(2026, 9, 15))
    source = config["source"]
    source.setdefault("station", "bee_")
    if not isinstance(source["station"], str) or not re.fullmatch(r"[a-z0-9_]{4}", source["station"]):
        raise PushError("source.station must contain four lowercase ASCII letters, digits or underscores")
    source.setdefault("year_base", 2000)
    integer(source["year_base"], "source.year_base", 1900, 9900)
    if source["year_base"] % 100:
        raise PushError("source.year_base must be a century, such as 2000")
    defaults = {
        "schedule": {"daily_utc": "00:10", "retry_seconds": 600},
        "network": {
            "connect_timeout_seconds": 30,
            "stall_timeout_seconds": 300,
            "completion_timeout_seconds": 300,
            "tcp_keepalive_idle_seconds": 60,
            "tcp_keepalive_interval_seconds": 30,
            "tcp_keepalive_probes": 5,
        },
        "compression": {"preset": 6, "threads": 2, "memory_limit_mib": 512},
    }
    for section, values in defaults.items():
        supplied = config.setdefault(section, {})
        object_fields(supplied, values)
        for key, value in values.items():
            supplied.setdefault(key, value)
    schedule = config["schedule"]
    if not isinstance(schedule["daily_utc"], str) or not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", schedule["daily_utc"]):
        raise PushError("schedule.daily_utc must be HH:MM in UTC")
    integer(schedule["retry_seconds"], "schedule.retry_seconds", 1, 86400)
    for key in defaults["network"]:
        upper = 127 if key == "tcp_keepalive_probes" else 32767 if key.startswith("tcp_keepalive_") else 86400
        integer(config["network"][key], f"network.{key}", 1, upper)
    compression = config["compression"]
    integer(compression["preset"], "compression.preset", 0, 9)
    integer(compression["threads"], "compression.threads", 1, 64)
    integer(compression["memory_limit_mib"], "compression.memory_limit_mib", 32, 1048576)
    return config


class TransferWatch:
    """The main thread closes sockets; only the worker drives FTP commands."""

    def __init__(self, stop, stall_seconds):
        self.stop = stop
        self.stall_seconds = stall_seconds
        self.lock = threading.Lock()
        self.sockets = set()
        self.last_progress = None
        self.reported_at = None
        self.reported_bytes = 0
        self.bytes = 0
        self.total_bytes = 0
        self.label = ""
        self.aborted = False

    @contextlib.contextmanager
    def track(self, sock, label=None, total_bytes=0, offset=0):
        with self.lock:
            self.sockets.add(sock)
            if label is not None:
                self.last_progress = time.monotonic()
                self.reported_at = self.last_progress
                self.reported_bytes = offset
                self.bytes = offset
                self.total_bytes = total_bytes
                self.label = label
                self.aborted = False
        try:
            self.check_stop()
            yield sock
        finally:
            if label is not None:
                self.progress()
            with self.lock:
                self.sockets.discard(sock)
                if label is not None:
                    self.last_progress = None
                    self.reported_at = None
            sock.close()

    def check_stop(self):
        if self.stop.is_set():
            raise PushError("Shutdown requested")

    def advance(self, count):
        self.check_stop()
        with self.lock:
            self.bytes += count
            self.last_progress = time.monotonic()

    def finish_data(self):
        self.progress()
        with self.lock:
            if self.aborted:
                raise PushError("Transfer cancelled after stalled progress")
            self.last_progress = None
            self.reported_at = None
        self.check_stop()

    def poll(self):
        with self.lock:
            stalled = self.last_progress is not None and time.monotonic() - self.last_progress >= self.stall_seconds
            if stalled and not self.aborted:
                LOG.error("No byte progress for %s seconds; disconnecting %s", self.stall_seconds, self.label)
                self.aborted = True
            sockets = list(self.sockets) if stalled or self.stop.is_set() else []
        for sock in sockets:
            with contextlib.suppress(OSError):
                sock.shutdown(socket.SHUT_RDWR)
            with contextlib.suppress(OSError):
                sock.close()

    def progress(self):
        with self.lock:
            if self.last_progress is not None:
                now = time.monotonic()
                elapsed = now - self.reported_at
                if elapsed <= 0:
                    return
                transferred = self.bytes - self.reported_bytes
                LOG.info(
                    "%s: %.2fMiB transferred, avg %.2f MiB/s, progress %.1f%%",
                    self.label,
                    self.bytes / (1024 * 1024),
                    transferred / elapsed / (1024 * 1024),
                    100 * self.bytes / self.total_bytes if self.total_bytes else 100.0,
                )
                self.reported_at = now
                self.reported_bytes = self.bytes


class SecureFTP(ftplib.FTP_TLS):
    def ntransfercmd(self, cmd, rest=None):
        connection, size = ftplib.FTP.ntransfercmd(self, cmd, rest)
        if self._prot_p:
            try:
                connection = self.context.wrap_socket(connection, server_hostname=self.host, session=self.sock.session)
            except BaseException:
                connection.close()
                raise
        return connection, size


class ImplicitTLS(SecureFTP):
    def connect(self, host="", port=990, timeout=-999, source_address=None):
        self.host, self.port, self.timeout = host, port, timeout
        raw = socket.create_connection((host, port), timeout, source_address)
        try:
            self.sock = self.context.wrap_socket(raw, server_hostname=host)
        except BaseException:
            raw.close()
            raise
        self.af = self.sock.family
        self.file = self.sock.makefile("r", encoding=self.encoding)
        self.welcome = self.getresp()
        return self.welcome


@contextlib.contextmanager
def transfer_phase(label, report_success=True):
    LOG.debug("%s", label)
    started = time.monotonic()
    try:
        yield
    except (OSError, EOFError, ftplib.Error) as exc:
        raise PushError(f"{label}: {exc}") from exc
    if report_success:
        LOG.info("%s completed in %.1f seconds", label, time.monotonic() - started)


def completion_reply(ftp, name, network):
    timeout = network["completion_timeout_seconds"]
    previous_timeout = ftp.sock.gettimeout()
    try:
        ftp.sock.settimeout(timeout)
        started = time.monotonic()
        with transfer_phase(f"Waiting for FTP transfer-complete reply for {name} (timeout {timeout}s)", report_success=False):
            reply = ftp.voidresp()
            LOG.info("FTP transfer-complete reply for %s: %s in %.1f seconds", name, reply[:3], time.monotonic() - started)
    finally:
        with contextlib.suppress(OSError):
            ftp.sock.settimeout(previous_timeout)


@contextlib.contextmanager
def connect(endpoint, network, watch, secure=False):
    if secure:
        if endpoint.get("verify_tls", True):
            context = ssl.create_default_context(cafile=endpoint.get("ca_file"))
        else:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
        cls = ImplicitTLS if endpoint["tls"] == "implicit" else SecureFTP
        ftp = cls(context=context)
    else:
        ftp = ftplib.FTP()
    try:
        watch.check_stop()
        ftp.connect(endpoint["host"], endpoint["port"], timeout=network["connect_timeout_seconds"])
        # AUTH TLS replaces the socket, so track the authenticated control socket below.
        ftp.login(endpoint["username"], endpoint["password"])
        if secure:
            ftp.prot_p()
        # The control connection is idle while the separate data socket is busy.
        # Linux TCP probes keep that connection active through NAT/firewalls.
        ftp.sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        ftp.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, network["tcp_keepalive_idle_seconds"])
        ftp.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, network["tcp_keepalive_interval_seconds"])
        ftp.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, network["tcp_keepalive_probes"])
        with watch.track(ftp.sock):
            ftp.voidcmd("TYPE I")
            yield ftp
    finally:
        ftp.close()


def names(ftp):
    """NLST avoids requiring MLSD support on the receiver."""
    return {name.rstrip("/").rsplit("/", 1)[-1] for name in ftp.nlst()}


def metadata(ftp, name):
    ftp.voidcmd("TYPE I")
    size = ftp.size(name)
    reply = ftp.sendcmd("MDTM " + name)
    stamp = reply[4:].strip()
    try:
        if not re.fullmatch(r"[0-9]{14}(?:\.[0-9]+)?", stamp):
            raise ValueError
        seconds, _, fraction = stamp.partition(".")
        parsed = datetime.strptime(seconds, "%Y%m%d%H%M%S").replace(tzinfo=timezone.utc)
    except ValueError as exc:
        raise PushError("Receiver returned an invalid MDTM timestamp") from exc
    if size is None or size < 0:
        raise PushError("Receiver did not return a valid SIZE")
    nanoseconds = int(parsed.timestamp()) * 1_000_000_000 + int((fraction + "000000000")[:9])
    return {"size": size, "mtime": stamp, "mtime_ns": nanoseconds}


def receive(ftp, name, output, watch, network, expected_size, offset=0):
    ftp.voidcmd("TYPE I")
    received = offset
    with watch.track(ftp.transfercmd("RETR " + name, rest=offset or None), "Download " + name, expected_size, offset) as data:
        data.settimeout(network["stall_timeout_seconds"])
        while True:
            block = data.recv(64 * 1024)
            if not block:
                break
            received += len(block)
            if received > expected_size:
                raise InvalidPartial("Download exceeds the advertised source size; discarding partial")
            output.write(block)
            watch.advance(len(block))
        if watch.aborted:
            raise PushError("Download cancelled after stalled progress")
    completion_reply(ftp, name, network)


def send(ftp, name, source, watch, network, total_bytes, display_name=None, offset=0):
    display_name = display_name or name
    ftp.voidcmd("TYPE I")
    source.seek(offset)
    with watch.track(ftp.transfercmd("STOR " + name, rest=offset or None), "Upload " + display_name, total_bytes, offset) as data:
        data.settimeout(network["stall_timeout_seconds"])
        while block := source.read(64 * 1024):
            view = memoryview(block)
            while view:
                count = data.send(view)
                if not count:
                    raise PushError("Upload connection stopped accepting bytes")
                watch.advance(count)
                view = view[count:]
        watch.finish_data()
        if isinstance(data, ssl.SSLSocket):
            # Complete TLS close_notify before reading the FTP completion reply.
            timeout = network["completion_timeout_seconds"]
            data.settimeout(timeout)
            with transfer_phase(f"TLS data-channel shutdown for {display_name} (timeout {timeout}s)"):
                data.unwrap().close()
        if watch.aborted:
            raise PushError("Upload cancelled after stalled progress")
    completion_reply(ftp, display_name, network)


def enter_directory(ftp, path):
    if path.startswith("/"):
        ftp.cwd("/")
    for part in path.split("/"):
        if not part or part == ".":
            continue
        try:
            ftp.cwd(part)
        except ftplib.error_perm as exc:
            if not str(exc).startswith("550"):
                raise
            ftp.mkd(part)
            ftp.cwd(part)


def optional_size(ftp, name):
    ftp.voidcmd("TYPE I")
    try:
        return ftp.size(name)
    except ftplib.error_perm as exc:
        if str(exc).startswith("550"):
            return None
        raise


ERRORS = (OSError, EOFError, ftplib.Error, PushError, DownloadError, lzma.LZMAError)


def file_identity(path):
    stat = path.stat()
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def receiver_day(name, year_base):
    if not re.fullmatch(r"[0-9]{5}", name):
        return None
    year = year_base + int(name[:2])
    try:
        day = date(year, 1, 1) + timedelta(days=int(name[2:]) - 1)
    except OverflowError:
        return None
    return day if day.year == year else None


def expected_name(station, day):
    return f"{station}{(day - date(day.year, 1, 1)).days + 1:03d}0.{day.year % 100:02d}_"


def utc_now():
    return datetime.now(timezone.utc)


def daily_time(now, schedule):
    hour, minute = map(int, schedule["daily_utc"].split(":"))
    return now.replace(hour=hour, minute=minute, second=0, microsecond=0)


class Mirror:
    def __init__(self, config, stop):
        self.config, self.stop = config, stop
        self.root = config["archive_root"]
        self.ensure_archive_directory(self.root)
        self.state_path = local_path(self.root, ".mosaic-push/state.json")
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.download_watch = TransferWatch(stop, config["network"]["stall_timeout_seconds"])
        self.push_watch = TransferWatch(stop, config["network"]["stall_timeout_seconds"])
        self.state_lock = threading.RLock()
        self.failed = False
        self.warning_days = set()
        self.storage_lock = threading.RLock()
        self.reservations = {}
        self.pinned = set()
        self.receiver_present = None
        self.job_finished = threading.Event()
        self.limit = None if config["storage"]["limit_gib"] is None else int(config["storage"]["limit_gib"] * 1024**3)
        self.source_id = json_digest({k: v for k, v in config["source"].items() if k != "password"})
        self.destination_ids = {
            name: json_digest({k: v for k, v in endpoint.items() if k not in ("enabled", "password", "ca_file")})
            for name, endpoint in config["destinations"].items()
            if endpoint["enabled"]
        }
        if self.state_path.exists():
            try:
                self.state = json.loads(self.state_path.read_text())
                if (
                    self.state["version"] not in (1, 2, 3)
                    or self.state["source_id"] != self.source_id
                    or not isinstance(self.state["files"], dict)
                ):
                    raise ValueError
                for relative, record in self.state["files"].items():
                    day = date.fromisoformat(record["day"])
                    if relative != self.relative(day) or not isinstance(record.get("uploads", {}), dict):
                        raise ValueError
                    if any(not isinstance(receipt, dict) for receipt in record.get("uploads", {}).values()):
                        raise ValueError
                    if record.get("uploaded") is not None and not isinstance(record["uploaded"], dict):
                        raise ValueError
            except (ValueError, KeyError, TypeError) as exc:
                raise PushError("Invalid mosaic state or source configuration differs from this archive") from exc
        else:
            self.state = {"version": 2, "source_id": self.source_id, "files": {}}
            self.save()

        if self.state["version"] == 1:
            for record in self.state["files"].values():
                previous = record.pop("uploaded", None)
                record["uploads"] = {
                    name: previous
                    for name, identity in self.destination_ids.items()
                    if previous and previous.get("destination") == identity
                }
            self.state["version"] = 2
            self.save()
            LOG.info("Migrated transfer state to per-destination completion records")

        if self.state["version"] != 3:
            self.state["version"] = 3
            self.save()
        for relative, record in list(self.state["files"].items()):
            if record.get("evicting"):
                self.finish_eviction(relative, record)

    @staticmethod
    def xz_budget(size):
        return size + max(64 * 1024**2, (size + 99) // 100)

    def usage(self):
        # Logical lengths count sparse files too; filesystem free space is checked separately.
        total = 0
        for directory, _, files in os.walk(self.root):
            for name in files:
                path = Path(directory) / name
                try:
                    if not path.is_symlink():
                        total += path.stat().st_size
                except FileNotFoundError:
                    pass
        return total

    def job_bytes(self, relative):
        archive = local_path(self.root, relative)
        raw = self.raw_path(relative)
        return sum(
            p.stat().st_size
            for p in (raw, raw.with_name(raw.name + ".part"), archive.with_name(archive.name + ".part"))
            if p.is_file()
        )

    def delivered(self, record):
        return all(
            (receipt := record.get("uploads", {}).get(name, {})).get("destination") == identity
            and receipt.get("archive") == record.get("archive")
            and receipt.get("sha512") == record.get("sha512")
            and record.get("sha512") is not None
            for name, identity in self.destination_ids.items()
        )

    def safe_to_evict(self, relative, record):
        archive = local_path(self.root, relative)
        if relative in self.pinned or record.get("evicted") or not archive.is_file():
            return False
        if file_identity(archive) != record.get("archive") or record.get("expected_source") != record.get("archive_source"):
            return False
        if self.job_bytes(relative):
            return False
        return self.delivered(record)

    def finish_eviction(self, relative, record):
        archive = local_path(self.root, relative)
        # A durable intent precedes unlink; recovery never mistakes eviction for a missing download.
        if archive.exists() and file_identity(archive) != record.get("archive"):
            raise PushError("Archive changed during eviction; refusing to delete it")
        archive.unlink(missing_ok=True)
        archive.with_suffix(".sha512").unlink(missing_ok=True)
        record.pop("evicting", None)
        record["evicted"] = record["archive_source"]
        record.pop("pending_uploads", None)
        self.save_record(relative, record)
        parent = archive.parent
        while parent != self.root:
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent
        LOG.info("Evicted local archive %s", relative)

    def reserve(self, relative, size):
        with self.storage_lock:
            budget = size + self.xz_budget(size) + 4096
            if self.limit is not None and budget + 256 * 1024**2 > self.limit:
                raise StorageBlocked("One raw file plus compression workspace exceeds storage.limit_gib; no archives were evicted")
            while True:
                outstanding = sum(
                    max(0, value - self.job_bytes(key)) for key, value in self.reservations.items() if key != relative
                )
                extra = max(0, budget - self.job_bytes(relative))
                margin = 256 * 1024**2
                fits = self.limit is None or self.usage() + outstanding + extra + margin <= self.limit
                if fits and shutil.disk_usage(self.root).free >= outstanding + extra + margin:
                    self.reservations[relative] = budget
                    return
                candidates = (
                    []
                    if self.limit is None
                    else [
                        (key, item)
                        for key, item in sorted(self.state["files"].items())
                        if key != relative and self.safe_to_evict(key, item)
                    ]
                )
                if not candidates:
                    raise StorageBlocked(
                        "Storage budget unavailable; retaining pending uploads and partial files, deferring new work"
                    )
                key, item = candidates[0]
                item = copy.deepcopy(item)
                item["evicting"] = True
                self.save_record(key, item)
                self.finish_eviction(key, item)

    def trim(self):
        if self.limit is None:
            return
        while self.usage() + 256 * 1024**2 > self.limit:
            candidates = [(key, record) for key, record in sorted(self.state["files"].items()) if self.safe_to_evict(key, record)]
            if not candidates:
                raise PushError("Storage remains above budget; pending or unmanaged files are protected")
            key, record = candidates[0]
            record = copy.deepcopy(record)
            record["evicting"] = True
            self.save_record(key, record)
            self.finish_eviction(key, record)

    def gc_evicted(self):
        if self.receiver_present is None:
            return
        for relative, record in list(self.state["files"].items()):
            if not record.get("evicted") or self.job_bytes(relative) or local_path(self.root, relative).exists():
                continue
            record["absent_scans"] = (
                0 if date.fromisoformat(record["day"]) in self.receiver_present else record.get("absent_scans", 0) + 1
            )
            if record["absent_scans"] >= 2:
                del self.state["files"][relative]
                LOG.info("Collected eviction record %s", relative)
            else:
                self.state["files"][relative] = record
        self.save()

    def checksum(self, relative, record):
        archive = local_path(self.root, relative)
        if file_identity(archive) != record.get("archive"):
            raise PushError("Archive changed before checksum publication")
        if not record.get("sha512"):
            record["sha512"] = self.hash_file(archive, compressed=True)
            if file_identity(archive) != record["archive"]:
                raise PushError("Archive changed while hashing")
            self.save_record(relative, record)
        content = f"{record['sha512']}  {archive.name.removesuffix('.xz')}\n".encode()
        sidecar = archive.with_suffix(".sha512")
        if not sidecar.exists() or sidecar.read_bytes() != content:
            temporary = sidecar.with_name(sidecar.name + ".part")
            try:
                with temporary.open("wb") as stream:
                    stream.write(content)
                    stream.flush()
                    os.fsync(stream.fileno())
                self.set_output_permissions(temporary)
                os.replace(temporary, sidecar)
            finally:
                temporary.unlink(missing_ok=True)
        return content

    def save(self):
        with self.state_lock:
            atomic_json(self.state_path, self.state)

    def save_record(self, relative, record):
        # Each file has one stage owner. Persist detached records so serialization
        # cannot race with another worker changing its own in-memory record.
        with self.state_lock:
            self.state["files"][relative] = copy.deepcopy(record)
            self.save()

    def poll(self):
        self.download_watch.poll()
        self.push_watch.poll()

    def progress(self):
        self.download_watch.progress()
        self.push_watch.progress()

    def relative(self, day):
        return f"{day:%Y/%m/%d}/{expected_name(self.config['source']['station'], day)}.xz"

    def raw_path(self, relative):
        return local_path(self.root, ".mosaic-push/raw/" + relative.removesuffix(".xz"))

    def error(self, label, exc):
        self.failed = True
        message = str(exc)
        for endpoint in [self.config["source"], *self.config["destinations"].values()]:
            secret = endpoint.get("password")
            if isinstance(secret, str) and secret:
                message = message.replace(secret, "<redacted>")
        LOG.error("%s: %s", label, message)

    def warn(self, key, message, *args):
        token = (utc_now().date(), key)
        if token not in self.warning_days:
            self.warning_days = {item for item in self.warning_days if item[0] == token[0]}
            self.warning_days.add(token)
            LOG.warning(message, *args)

    def session(self, destination=None):
        if destination is None:
            return connect(self.config["source"], self.config["network"], self.download_watch)
        endpoint = self.config["destinations"][destination]
        if not endpoint["enabled"]:
            raise PushError(f"FTPS forwarding is disabled for {destination}")
        return connect(endpoint, self.config["network"], self.push_watch, secure=True)

    def inventory(self):
        source = self.config["source"]
        now = utc_now()
        cutoff = now.date() - timedelta(days=1 if now >= daily_time(now, self.config["schedule"]) else 2)
        found = {}
        self.receiver_present = None
        present = set()
        complete = True
        with self.session() as ftp:
            ftp.cwd(source["path"])
            prefix = ftp.pwd()
            directories = sorted((day, name) for name in names(ftp) if (day := receiver_day(name, source["year_base"])) is not None)
            if not directories:
                self.warn("empty", "Receiver has no valid YYDDD directories")
                self.receiver_present = set()
                return found
            excluded = {day for day, _ in directories[:2]}
            available = {day for day, _ in directories}
            # Do not infer missing history before the receiver's oldest retained day.
            day = directories[0][0]
            while day <= cutoff:
                if day not in available and not self.has_archive(day):
                    self.warn(str(day), "Missing receiver date directory: %s", day)
                day += timedelta(days=1)
            for day, directory in directories:
                self.download_watch.check_stop()
                name = expected_name(source["station"], day)
                try:
                    ftp.cwd(prefix + "/" + directory)
                    listing = names(ftp)
                    if name in listing or name + ".A" in listing:
                        present.add(day)
                    if day in excluded or day > cutoff:
                        continue
                    if name + ".A" in listing:
                        continue
                    if name not in listing:
                        if not self.has_archive(day):
                            self.warn(str(day), "Missing closed SBF in receiver directory %s", directory)
                        continue
                    found[day] = metadata(ftp, name)
                except ERRORS as exc:
                    complete = False
                    self.error(f"Cannot inspect receiver date {day}", exc)
                    # A dropped connection cannot be reused for the rest of this inventory.
                    try:
                        ftp.voidcmd("NOOP")
                    except ERRORS:
                        break
        if complete:
            self.receiver_present = present
        return found

    def has_archive(self, day):
        relative = self.relative(day)
        record = self.state["files"].get(relative, {})
        return bool(record.get("archive") and local_path(self.root, relative).is_file())

    def source_directory(self, day):
        return self.config["source"]["path"].rstrip("/") + "/" + expand_path("%y%j", day)

    def download(self, day, identity, relative, record):
        raw = self.raw_path(relative)
        if record.get("raw_source") == identity and raw.is_file() and file_identity(raw) == record.get("raw"):
            return
        raw.parent.mkdir(parents=True, exist_ok=True)
        part = raw.with_name(raw.name + ".part")
        name = expected_name(self.config["source"]["station"], day)
        LOG.info("Downloading %s (%d bytes)", name, identity["size"])
        if record.get("partial_source") != identity:
            part.unlink(missing_ok=True)
            record["partial_source"] = identity
            record.pop("download_no_rest", None)
            self.save_record(relative, record)
        offset = part.stat().st_size if part.exists() else 0
        if offset > identity["size"] or record.get("download_no_rest"):
            part.unlink(missing_ok=True)
            offset = 0
        with self.session() as ftp:
            ftp.cwd(self.source_directory(day))
            if name + ".A" in names(ftp) or metadata(ftp, name) != identity:
                raise PushError("Source changed or became active before download")
            if offset < identity["size"] or not part.exists():
                LOG.info("Download %s starting at byte %d", name, offset)
                try:
                    with part.open("ab" if offset else "wb") as output:
                        receive(ftp, name, output, self.download_watch, self.config["network"], identity["size"], offset)
                        output.flush()
                        os.fsync(output.fileno())
                except InvalidPartial:
                    part.unlink(missing_ok=True)
                    record.pop("partial_source", None)
                    self.save_record(relative, record)
                    raise
                except ftplib.error_perm as exc:
                    if offset and str(exc)[:3] in ("500", "501", "502", "504"):
                        record["download_no_rest"] = True
                        self.save_record(relative, record)
                        LOG.warning("Receiver rejected REST; next download attempt starts at zero")
                    raise
            if part.stat().st_size != identity["size"] or metadata(ftp, name) != identity or name + ".A" in names(ftp):
                raise PushError("Source changed or downloaded size differs")
        digest = self.hash_file(part)
        os.utime(part, ns=(identity["mtime_ns"], identity["mtime_ns"]))
        os.replace(part, raw)
        record.update(raw_source=identity, raw=file_identity(raw), raw_sha512=digest)
        record.pop("partial_source", None)
        self.save_record(relative, record)

    def hash_file(self, path, compressed=False):
        digest = hashlib.sha512()
        opener = lzma.open if compressed else open
        with opener(path, "rb") as stream:
            while block := stream.read(1024 * 1024):
                self.download_watch.check_stop()
                digest.update(block)
        return digest.hexdigest()

    def ensure_archive_directory(self, path):
        """Set permissions only on archive directories created by this call."""
        if path == self.root:
            path.parent.mkdir(parents=True, exist_ok=True)
        elif not path.parent.exists():
            self.ensure_archive_directory(path.parent)
        try:
            path.mkdir()
        except FileExistsError:
            if not path.is_dir():
                raise
        else:
            self.set_output_permissions(path, directory=True)

    def set_output_permissions(self, path, directory=False):
        """Apply explicit permissions to a verified archive before publication."""
        output = self.config["output"]
        if output["group"] is not None:
            os.chown(path, -1, output["group"])
        mode = output["dirmode" if directory else "mode"]
        if mode is not None:
            os.chmod(path, mode)

    def compress(self, relative, record):
        raw = self.raw_path(relative)
        if not raw.is_file() or file_identity(raw) != record.get("raw"):
            raise PushError("Downloaded SBF is missing or changed before compression")
        if not record.get("raw_sha512"):
            record["raw_sha512"] = self.hash_file(raw)
            self.save_record(relative, record)
        archive = local_path(self.root, relative)
        if record.get("adopt_existing") and archive.exists():
            LOG.info("Verifying existing archive against downloaded source: %s", relative)
            before = file_identity(archive)
            if self.hash_file(archive, compressed=True) != record["raw_sha512"] or file_identity(archive) != before:
                raise PushError("Existing unmanaged xz differs from the receiver; keeping both archive and downloaded raw copy")
            record.update(archive=before, archive_source=record["raw_source"], sha512=record["raw_sha512"])
            record.pop("adopt_existing", None)
            self.save_record(relative, record)
            self.checksum(relative, record)
            self.remove_raw(relative, record)
            return
        self.ensure_archive_directory(archive.parent)
        part = archive.with_name(archive.name + ".part")
        options = self.config["compression"]
        command = [
            "xz",
            "--stdout",
            f"-{options['preset']}",
            f"--threads={options['threads']}",
            f"--memlimit-compress={options['memory_limit_mib']}MiB",
            "--",
            str(raw),
        ]
        environment = {k: v for k, v in os.environ.items() if k not in ("XZ_OPT", "XZ_DEFAULTS")}
        LOG.info("Compressing and verifying %s", raw.name)
        try:
            # Bound every write, including incompressible input; never let xz overrun its reservation.
            with part.open("wb") as output:
                with subprocess.Popen(command, stdout=subprocess.PIPE, bufsize=0, env=environment) as process:
                    try:
                        written = 0
                        with selectors.DefaultSelector() as selector:
                            selector.register(process.stdout, selectors.EVENT_READ)
                            while True:
                                self.download_watch.check_stop()
                                if not selector.select(0.25):
                                    continue
                                block = process.stdout.read(64 * 1024)
                                if not block:
                                    break
                                written += len(block)
                                if written > self.xz_budget(record["raw"]["size"]):
                                    raise PushError("xz output exceeded reserved space; keeping raw for retry")
                                output.write(block)
                        if process.wait():
                            raise PushError(f"xz exited with status {process.returncode}")
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
                output.flush()
                os.fsync(output.fileno())
            if self.hash_file(part, compressed=True) != record["raw_sha512"]:
                raise PushError("Decompressed xz content does not match downloaded SBF")
            os.utime(part, ns=(record["raw_source"]["mtime_ns"], record["raw_source"]["mtime_ns"]))
            self.set_output_permissions(part)
            os.replace(part, archive)
            record.update(archive=file_identity(archive), archive_source=record["raw_source"], sha512=record["raw_sha512"])
            record.pop("adopt_existing", None)
            self.save_record(relative, record)
            self.checksum(relative, record)
            # Only delete this tool's private downloaded copy, never receiver files.
            self.remove_raw(relative, record)
        finally:
            part.unlink(missing_ok=True)

    def remove_raw(self, relative, record):
        self.raw_path(relative).unlink(missing_ok=True)
        for key in ("raw_source", "raw", "raw_sha256", "raw_sha512"):
            record.pop(key, None)
        self.save_record(relative, record)

    def retry_identity(self, record, key):
        if key.startswith("push:"):
            return {"endpoint": self.destination_ids[key[5:]], "archive": record.get("archive")}
        return record.get("expected_source")

    def retry_due(self, record, key):
        retry = record.get("retries", {}).get(key, {})
        if retry.get("identity") == self.retry_identity(record, key) and retry.get("after", 0) > time.time():
            self.failed = True
            LOG.info("Deferring %s until retry backoff expires", key)
            return False
        return True

    def retry_failed(self, relative, record, key):
        identity = self.retry_identity(record, key)
        previous = record.setdefault("retries", {}).get(key, {})
        count = min(previous.get("count", 0) + 1, 10) if previous.get("identity") == identity else 1
        delay = min(3600, self.config["schedule"]["retry_seconds"] * 2 ** (count - 1))
        record["retries"][key] = {"identity": identity, "count": count, "after": time.time() + delay}
        self.save_record(relative, record)

    def push_file(self, relative, record):
        self.checksum(relative, record)
        # One worker visits enabled targets in configuration order for each file.
        # A target failure leaves its receipt pending but never suppresses the rest.
        for name in self.destination_ids:
            if self.stop.is_set():
                return
            key = "push:" + name
            if not self.retry_due(record, key):
                continue
            try:
                self.upload(relative, record, name)
                if record.get("retries", {}).pop(key, None):
                    self.save_record(relative, record)
            except Exception as exc:
                self.retry_failed(relative, record, key)
                self.error(f"push [{name}] failed for {relative}", exc)

    def upload(self, relative, record, destination):
        endpoint = self.config["destinations"][destination]
        archive = local_path(self.root, relative)
        if not archive.is_file():
            if not record.get("uploads"):
                self.warn(relative, "Local archive missing before upload: %s", relative)
                self.failed = True
            return
        identity = file_identity(archive)
        if identity != record.get("archive"):
            raise PushError("Managed local archive changed; refusing to upload unverified content")
        receipt = {
            "destination": self.destination_ids[destination],
            "archive": identity,
            "checked_date": utc_now().date().isoformat(),
            "sha512": record["sha512"],
        }
        if record.get("uploads", {}).get(destination) == receipt:
            return
        directory = expand_path(endpoint["path"], date.fromisoformat(record["day"]))
        with self.session(destination) as ftp:
            enter_directory(ftp, directory)
            with transfer_phase(f"Checking final remote size for [{destination}] {relative}"):
                remote_size = optional_size(ftp, archive.name)
            pending = record.setdefault("pending_uploads", {}).get(destination)
            transfer_id = {"destination": self.destination_ids[destination], "archive": identity, "sha512": record["sha512"]}
            previous = record.get("uploads", {}).get(destination, {})
            changed = previous.get("archive") is not None and previous.get("archive") != identity
            if pending and pending.get("identity") == transfer_id and pending.get("published"):
                changed = False
            if remote_size != identity["size"] or changed:
                temporary = archive.name + ".ngo-mosaic-push.part"
                offset = optional_size(ftp, temporary) if pending and pending.get("identity") == transfer_id else 0
                offset = offset or 0
                no_rest = bool(pending and pending.get("no_rest"))
                if offset > identity["size"] or no_rest:
                    offset = 0
                record["pending_uploads"][destination] = {"identity": transfer_id, "no_rest": no_rest}
                self.save_record(relative, record)
                if offset < identity["size"] or identity["size"] == 0:
                    LOG.info("Uploading [%s] %s starting at byte %d", destination, relative, offset)
                    try:
                        with archive.open("rb") as stream:
                            send(
                                ftp,
                                temporary,
                                stream,
                                self.push_watch,
                                self.config["network"],
                                identity["size"],
                                display_name=f"[{destination}] {temporary}",
                                offset=offset,
                            )
                    except ftplib.error_perm as exc:
                        if offset and str(exc)[:3] in ("500", "501", "502", "504"):
                            record["pending_uploads"][destination]["no_rest"] = True
                            self.save_record(relative, record)
                            LOG.warning("Target [%s] rejected REST; next upload attempt starts at zero", destination)
                        raise
                if file_identity(archive) != identity or optional_size(ftp, temporary) != identity["size"]:
                    raise PushError("Uploaded size mismatch or local archive changed")
                ftp.rename(temporary, archive.name)
                if optional_size(ftp, archive.name) != identity["size"]:
                    raise PushError("Published remote size mismatch")
                # Persist archive publication separately so a checksum retry does not retransmit the xz.
                record["pending_uploads"][destination]["published"] = True
                self.save_record(relative, record)
            else:
                LOG.info("Remote size matches [%s]; skipping %s", destination, relative)
            content = self.checksum(relative, record)
            sidecar = archive.with_suffix(".sha512").name
            temporary = sidecar + ".ngo-mosaic-push.part"
            send(
                ftp,
                temporary,
                io.BytesIO(content),
                self.push_watch,
                self.config["network"],
                len(content),
                display_name=f"[{destination}] {temporary}",
            )
            if optional_size(ftp, temporary) != len(content):
                raise PushError("Uploaded checksum size mismatch")
            ftp.rename(temporary, sidecar)
            if optional_size(ftp, sidecar) != len(content):
                raise PushError("Published checksum size mismatch")
        record.get("pending_uploads", {}).pop(destination, None)
        record.setdefault("uploads", {})[destination] = receipt
        self.save_record(relative, record)

    def enqueue(self, work_queue, job):
        while not self.stop.is_set():
            try:
                work_queue.put(job, timeout=0.25)
                return True
            except queue.Full:
                continue
        return False

    def consume(self, work_queue, action, stage):
        while not self.stop.is_set():
            try:
                job = work_queue.get(timeout=0.25)
            except queue.Empty:
                continue
            if job is None:
                return
            relative, record = job
            try:
                action(relative, record)
            except Exception as exc:
                self.error(f"{stage} failed for {relative}", exc)
                try:
                    if stage == "download" and not isinstance(exc, StorageBlocked):
                        self.retry_failed(relative, record, "download")
                except Exception as state_error:
                    self.error("Cannot persist retry state", state_error)
                finally:
                    self.finish_job(relative)
            else:
                if stage == "push":
                    self.finish_job(relative)

    def plan(self, inventory):
        """Assign each file to exactly one initial stage before workers start."""
        records = copy.deepcopy(self.state["files"])
        jobs = []
        for day, identity in inventory.items():
            relative = self.relative(day)
            is_new = relative not in records
            record = records.setdefault(
                relative, {"day": day.isoformat(), "adopt_existing": local_path(self.root, relative).exists()}
            )
            if is_new or record.get("expected_source") != identity:
                record["expected_source"] = identity
                self.save_record(relative, record)
        for relative, record in sorted(records.items()):
            self.download_watch.check_stop()
            day = date.fromisoformat(record["day"])
            archive = local_path(self.root, relative)
            raw = self.raw_path(relative)
            identity = inventory.get(day)
            if record.get("evicted"):
                if identity is None or identity == record["evicted"]:
                    continue
                record.pop("evicted", None)
                record.pop("absent_scans", None)
                self.save_record(relative, record)
            archive_ready = archive.is_file() and file_identity(archive) == record.get("archive")
            if identity is not None and (not archive_ready or record.get("archive_source") != identity):
                stage = "push" if archive_ready and not self.delivered(record) else "download"
            elif record.get("raw_source") and raw.is_file():
                stage = "compress"
            elif record.get("archive"):
                stage = "push"
            else:
                self.warn(relative, "Unfinished source is no longer downloadable: %s", relative)
                self.failed = True
                continue
            jobs.append((stage, (relative, record)))
        return jobs

    def finish_job(self, relative):
        with self.storage_lock:
            self.reservations.pop(relative, None)
            self.pinned.discard(relative)
        self.job_finished.set()

    def cycle(self):
        self.failed = False
        self.receiver_present = None
        try:
            inventory = self.inventory()
        except ERRORS as exc:
            self.error("Receiver inventory failed", exc)
            inventory = {}
        self.gc_evicted()
        jobs = self.plan(inventory)
        # Finish existing archives first so failed targets can recover before storage admission.
        jobs.sort(key=lambda item: (item[0] != "push", item[1][0]))
        downloads = queue.Queue()
        # Paths/metadata only. Backpressure bounds newly downloaded raw backlog to
        # one active compression, two queued files, and the current download.
        compressions = queue.Queue(maxsize=2)
        pushes = queue.Queue()

        def download_job(relative, record):
            if not self.retry_due(record, "download"):
                self.finish_job(relative)
                return
            if record.get("raw_source") != record["expected_source"]:
                self.raw_path(relative).unlink(missing_ok=True)
                for key in ("raw", "raw_source", "raw_sha512", "raw_sha256"):
                    record.pop(key, None)
            if record.get("partial_source") != record["expected_source"]:
                raw = self.raw_path(relative)
                raw.with_name(raw.name + ".part").unlink(missing_ok=True)
            self.reserve(relative, record["expected_source"]["size"])
            self.download(date.fromisoformat(record["day"]), record["expected_source"], relative, record)
            record.get("retries", {}).pop("download", None)
            self.save_record(relative, record)
            self.enqueue(compressions, (relative, record))

        def compress_job(relative, record):
            if relative not in self.reservations:
                self.reserve(relative, record["raw"]["size"])
            archive = local_path(self.root, relative)
            if (
                record.get("archive_source") == record.get("raw_source")
                and archive.is_file()
                and file_identity(archive) == record.get("archive")
            ):
                self.remove_raw(relative, record)
            else:
                self.compress(relative, record)
            self.enqueue(pushes, (relative, record))

        stages = [
            ("download", downloads, download_job),
            ("compress", compressions, compress_job),
            ("push", pushes, self.push_file),
        ]
        workers = []
        try:
            for name, work_queue, action in stages:
                thread = threading.Thread(target=self.consume, args=(work_queue, action, name), name="mosaic-" + name)
                thread.start()
                workers.append((work_queue, thread))
            queues = {name: work_queue for name, work_queue, _ in stages}
            for stage, job in jobs:
                self.job_finished.clear()
                with self.storage_lock:
                    self.pinned.add(job[0])
                if not self.enqueue(queues[stage], job):
                    break
                if self.limit is not None:
                    while not self.job_finished.wait(0.25):
                        if self.stop.is_set():
                            break
                    if self.stop.is_set():
                        break
        finally:
            # Finish upstream producers before terminating downstream consumers.
            # On shutdown, consumers exit themselves and complete files stay on disk.
            for work_queue, thread in workers:
                self.enqueue(work_queue, None)
                thread.join()
        try:
            self.trim()
        except ERRORS as exc:
            self.error("Storage cleanup deferred", exc)
        LOG.info("Scan complete%s", " with failures; retry pending" if self.failed else "")

    def worker(self):
        try:
            self.cycle()
        except Exception as exc:
            self.error("Scan aborted", exc)


def run(config, once):
    if shutil.which("xz") is None:
        raise PushError("xz must be installed and available in PATH")
    stop = threading.Event()
    old_handlers = {sig: signal.signal(sig, lambda signum, frame: stop.set()) for sig in (signal.SIGTERM, signal.SIGINT)}
    try:
        with root_lock(config["archive_root"]):
            mirror = Mirror(config, stop)
            while not stop.is_set():
                worker = threading.Thread(target=mirror.worker, name="mosaic-coordinator")
                worker.start()
                last_report = time.monotonic()
                while worker.is_alive():
                    mirror.poll()
                    if time.monotonic() - last_report >= 60:
                        mirror.progress()
                        last_report = time.monotonic()
                    worker.join(timeout=0.25)
                if once:
                    return 1 if mirror.failed or stop.is_set() else 0
                now = utc_now()
                next_daily = daily_time(now, config["schedule"])
                if next_daily <= now:
                    next_daily += timedelta(days=1)
                delay = min(config["schedule"]["retry_seconds"], (next_daily - now).total_seconds())
                stop.wait(delay)
        return 0
    finally:
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)


@click.command()
@click.option(
    "--config", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True, help="Single JSON configuration file."
)
@click.option("--once", is_flag=True, help="Run one scan/recovery cycle, then exit; keep daily eligibility rules.")
@click.option("--xz-threads", type=click.IntRange(1, 64), help="Override compression.threads from the JSON configuration.")
@click.option("--check-config", is_flag=True, help="Validate configuration without network or filesystem writes.")
def cli(config, once, xz_threads, check_config):
    """Mirror closed mosaic daily SBF files to local xz archives and optional named FTPS targets."""
    console = Console(stderr=True)
    handler = RichHandler(console=console, markup=False, show_path=False) if console.is_terminal else logging.StreamHandler()
    logging.basicConfig(
        level=logging.INFO, format="%(message)s" if console.is_terminal else "%(levelname)s %(message)s", handlers=[handler]
    )
    try:
        settings = read_config(config)
        if xz_threads is not None:
            settings["compression"]["threads"] = xz_threads
        if check_config:
            click.echo("Configuration is valid.")
            return
        raise click.exceptions.Exit(run(settings, once))
    except ERRORS as exc:
        raise click.ClickException(str(exc)) from exc


if __name__ == "__main__":
    cli()
