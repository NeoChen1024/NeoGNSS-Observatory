#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Mirror closed mosaic daily SBF files, compress locally, and publish via FTPS."""

import copy
import ftplib
import hashlib
import json
import logging
import lzma
import os
import queue
import re
import shutil
import signal
import subprocess
import threading
import time
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

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
from .mosaic_push_config import PushError, expand_path, read_config
from .mosaic_push_ftp import (
    TransferWatch,
    connect,
    enter_directory,
    metadata,
    names,
    optional_size,
    receive,
    send,
    transfer_phase,
)

LOG = logging.getLogger("neognss_observatory.mosaic_push")
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
        self.state_path = local_path(self.root, ".mosaic-push/state.json")
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.download_watch = TransferWatch(stop, config["network"]["stall_timeout_seconds"])
        self.push_watch = TransferWatch(stop, config["network"]["stall_timeout_seconds"])
        self.state_lock = threading.RLock()
        self.failed = False
        self.warning_days = set()
        self.source_id = json_digest({k: v for k, v in config["source"].items() if k != "password"})
        self.destination_id = (
            json_digest({k: v for k, v in config["destination"].items() if k not in ("enabled", "password", "ca_file")})
            if config["destination"]["enabled"]
            else None
        )
        if self.state_path.exists():
            try:
                self.state = json.loads(self.state_path.read_text())
                if (
                    self.state["version"] != 1
                    or self.state["source_id"] != self.source_id
                    or not isinstance(self.state["files"], dict)
                ):
                    raise ValueError
                for relative, record in self.state["files"].items():
                    day = date.fromisoformat(record["day"])
                    if relative != self.relative(day):
                        raise ValueError
            except (ValueError, KeyError, TypeError) as exc:
                raise PushError("Invalid mosaic state or source configuration differs from this archive") from exc
        else:
            self.state = {"version": 1, "source_id": self.source_id, "files": {}}
            self.save()

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
        for endpoint in ("source", "destination"):
            secret = self.config[endpoint].get("password")
            if isinstance(secret, str) and secret:
                message = message.replace(secret, "<redacted>")
        LOG.error("%s: %s", label, message)

    def warn(self, key, message, *args):
        token = (utc_now().date(), key)
        if token not in self.warning_days:
            self.warning_days = {item for item in self.warning_days if item[0] == token[0]}
            self.warning_days.add(token)
            LOG.warning(message, *args)

    def session(self, destination=False):
        if destination and not self.config["destination"]["enabled"]:
            raise PushError("FTPS forwarding is disabled")
        watch = self.push_watch if destination else self.download_watch
        return connect(self.config["destination" if destination else "source"], self.config["network"], watch, destination)

    def inventory(self):
        source = self.config["source"]
        now = utc_now()
        cutoff = now.date() - timedelta(days=1 if now >= daily_time(now, self.config["schedule"]) else 2)
        found = {}
        with self.session() as ftp:
            ftp.cwd(source["path"])
            prefix = ftp.pwd()
            directories = sorted((day, name) for name in names(ftp) if (day := receiver_day(name, source["year_base"])) is not None)
            if not directories:
                self.warn("empty", "Receiver has no valid YYDDD directories")
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
                if day in excluded or day > cutoff:
                    continue
                name = expected_name(source["station"], day)
                try:
                    ftp.cwd(prefix + "/" + directory)
                    listing = names(ftp)
                    if name + ".A" in listing:
                        continue
                    if name not in listing:
                        if not self.has_archive(day):
                            self.warn(str(day), "Missing closed SBF in receiver directory %s", directory)
                        continue
                    found[day] = metadata(ftp, name)
                except ERRORS as exc:
                    self.error(f"Cannot inspect receiver date {day}", exc)
                    # A dropped connection cannot be reused for the rest of this inventory.
                    try:
                        ftp.voidcmd("NOOP")
                    except ERRORS:
                        break
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
        try:
            with self.session() as ftp:
                ftp.cwd(self.source_directory(day))
                if name + ".A" in names(ftp) or metadata(ftp, name) != identity:
                    raise PushError("Source changed or became active before download")
                with part.open("wb") as output:
                    receive(ftp, name, output, self.download_watch, self.config["network"], identity["size"])
                    output.flush()
                    os.fsync(output.fileno())
                if part.stat().st_size != identity["size"] or metadata(ftp, name) != identity or name + ".A" in names(ftp):
                    raise PushError("Source changed or downloaded size differs")
            digest = self.hash_file(part)
            os.utime(part, ns=(identity["mtime_ns"], identity["mtime_ns"]))
            os.replace(part, raw)
            record.update(raw_source=identity, raw=file_identity(raw), raw_sha256=digest)
            self.save_record(relative, record)
        finally:
            part.unlink(missing_ok=True)

    def hash_file(self, path, compressed=False):
        digest = hashlib.sha256()
        opener = lzma.open if compressed else open
        with opener(path, "rb") as stream:
            while block := stream.read(1024 * 1024):
                self.download_watch.check_stop()
                digest.update(block)
        return digest.hexdigest()

    def compress(self, relative, record):
        raw = self.raw_path(relative)
        if not raw.is_file() or file_identity(raw) != record.get("raw"):
            raise PushError("Downloaded SBF is missing or changed before compression")
        archive = local_path(self.root, relative)
        if record.get("adopt_existing") and archive.exists():
            LOG.info("Verifying existing archive against downloaded source: %s", relative)
            before = file_identity(archive)
            if self.hash_file(archive, compressed=True) != record["raw_sha256"] or file_identity(archive) != before:
                raise PushError("Existing unmanaged xz differs from the receiver; keeping both archive and downloaded raw copy")
            record.update(archive=before, archive_source=record["raw_source"])
            record.pop("adopt_existing", None)
            self.save_record(relative, record)
            self.remove_raw(relative, record)
            return
        archive.parent.mkdir(parents=True, exist_ok=True)
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
            # stderr is inherited by the service journal, not captured in an unbounded pipe.
            with part.open("wb") as output:
                with subprocess.Popen(command, stdout=output, env=environment) as process:
                    while process.poll() is None:
                        if self.stop.wait(0.25):
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                            raise PushError("Compression interrupted")
                    if process.returncode:
                        raise PushError(f"xz exited with status {process.returncode}")
                output.flush()
                os.fsync(output.fileno())
            if self.hash_file(part, compressed=True) != record["raw_sha256"]:
                raise PushError("Decompressed xz content does not match downloaded SBF")
            os.utime(part, ns=(record["raw_source"]["mtime_ns"], record["raw_source"]["mtime_ns"]))
            os.replace(part, archive)
            record.update(archive=file_identity(archive), archive_source=record["raw_source"])
            record.pop("adopt_existing", None)
            record.pop("uploaded", None)
            self.save_record(relative, record)
            # Only delete this tool's private downloaded copy, never receiver files.
            self.remove_raw(relative, record)
        finally:
            part.unlink(missing_ok=True)

    def remove_raw(self, relative, record):
        self.raw_path(relative).unlink(missing_ok=True)
        for key in ("raw_source", "raw", "raw_sha256"):
            record.pop(key, None)
        self.save_record(relative, record)

    def upload(self, relative, record):
        if not self.config["destination"]["enabled"]:
            return
        archive = local_path(self.root, relative)
        if not archive.is_file():
            if not record.get("uploaded"):
                self.warn(relative, "Local archive missing before upload: %s", relative)
                self.failed = True
            return
        identity = file_identity(archive)
        if identity != record.get("archive"):
            raise PushError("Managed local archive changed; refusing to upload unverified content")
        if record.get("expected_source") != record.get("archive_source"):
            raise PushError("A newer source version is pending; retaining the previous local archive without uploading it")
        receipt = {"destination": self.destination_id, "archive": identity, "checked_date": utc_now().date().isoformat()}
        if record.get("uploaded") == receipt:
            return
        endpoint = self.config["destination"]
        directory = expand_path(endpoint["path"], date.fromisoformat(record["day"]))
        with self.session(destination=True) as ftp:
            enter_directory(ftp, directory)
            with transfer_phase(f"Checking final remote size for {relative}"):
                remote_size = optional_size(ftp, archive.name)
            if remote_size != identity["size"]:
                temporary = archive.name + ".ngo-mosaic-push.part"
                LOG.info("Uploading %s (%d bytes)", relative, identity["size"])
                with archive.open("rb") as stream:
                    send(ftp, temporary, stream, self.push_watch, self.config["network"])
                with transfer_phase(f"Verifying uploaded temporary size for {relative}"):
                    if file_identity(archive) != identity or optional_size(ftp, temporary) != identity["size"]:
                        raise PushError("Uploaded size mismatch or local archive changed")
                with transfer_phase(f"Publishing remote archive by rename for {relative}"):
                    ftp.rename(temporary, archive.name)
                with transfer_phase(f"Verifying published remote size for {relative}"):
                    if optional_size(ftp, archive.name) != identity["size"]:
                        raise PushError("Published remote size mismatch")
            else:
                LOG.info("Remote size matches; skipping %s", relative)
        record["uploaded"] = receipt
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
            archive_ready = archive.is_file() and file_identity(archive) == record.get("archive")
            if identity is not None and (not archive_ready or record.get("archive_source") != identity):
                stage = "download"
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

    def cycle(self):
        self.failed = False
        try:
            inventory = self.inventory()
        except ERRORS as exc:
            self.error("Receiver inventory failed", exc)
            inventory = {}
        jobs = self.plan(inventory)
        downloads = queue.Queue()
        # Paths/metadata only. Backpressure bounds newly downloaded raw backlog to
        # one active compression, two queued files, and the current download.
        compressions = queue.Queue(maxsize=2)
        pushes = queue.Queue()

        def download_job(relative, record):
            self.download(date.fromisoformat(record["day"]), record["expected_source"], relative, record)
            self.enqueue(compressions, (relative, record))

        def compress_job(relative, record):
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
            ("push", pushes, self.upload),
        ]
        workers = []
        try:
            for name, work_queue, action in stages:
                thread = threading.Thread(target=self.consume, args=(work_queue, action, name), name="mosaic-" + name)
                thread.start()
                workers.append((work_queue, thread))
            queues = {name: work_queue for name, work_queue, _ in stages}
            for stage, job in jobs:
                if not self.enqueue(queues[stage], job):
                    break
        finally:
            # Finish upstream producers before terminating downstream consumers.
            # On shutdown, consumers exit themselves and complete files stay on disk.
            for work_queue, thread in workers:
                self.enqueue(work_queue, None)
                thread.join()
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
    """Mirror closed mosaic daily SBF files to local xz archives and one FTPS server."""
    logging.basicConfig(
        level=logging.INFO, format="%(message)s", handlers=[RichHandler(console=Console(stderr=True), markup=False)]
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
