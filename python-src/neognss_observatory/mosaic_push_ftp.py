# SPDX-License-Identifier: GPL-3.0-only
"""Bounded FTP/FTPS operations with an externally cancellable data connection."""

import contextlib
import ftplib
import logging
import re
import socket
import ssl
import threading
import time
from datetime import datetime, timezone

from .mosaic_push_config import PushError

LOG = logging.getLogger(__name__)


class TransferWatch:
    """The main thread closes sockets; only the worker drives FTP commands."""

    def __init__(self, stop, stall_seconds):
        self.stop = stop
        self.stall_seconds = stall_seconds
        self.lock = threading.Lock()
        self.sockets = set()
        self.last_progress = None
        self.started = None
        self.bytes = 0
        self.label = ""
        self.aborted = False

    @contextlib.contextmanager
    def track(self, sock, label=None):
        with self.lock:
            self.sockets.add(sock)
            if label is not None:
                self.last_progress = time.monotonic()
                self.started = self.last_progress
                self.bytes = 0
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
                    self.started = None
            sock.close()

    def check_stop(self):
        if self.stop.is_set():
            raise PushError("Shutdown requested")

    def advance(self, count):
        self.check_stop()
        with self.lock:
            self.bytes += count
            self.last_progress = time.monotonic()

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
                elapsed = max(time.monotonic() - self.started, 0.000001)
                LOG.info(
                    "%s: %d bytes transferred, average %.2f MiB/s over %.1f seconds",
                    self.label,
                    self.bytes,
                    self.bytes / elapsed / (1024 * 1024),
                    elapsed,
                )


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


def receive(ftp, name, output, watch, network, expected_size):
    ftp.voidcmd("TYPE I")
    received = 0
    with watch.track(ftp.transfercmd("RETR " + name), "Download " + name) as data:
        data.settimeout(network["stall_timeout_seconds"])
        while True:
            block = data.recv(64 * 1024)
            if not block:
                break
            received += len(block)
            if received > expected_size:
                raise PushError("Download exceeds the advertised source size")
            output.write(block)
            watch.advance(len(block))
        if watch.aborted:
            raise PushError("Download cancelled after stalled progress")
    ftp.voidresp()


def send(ftp, name, source, watch, network):
    ftp.voidcmd("TYPE I")
    with watch.track(ftp.transfercmd("STOR " + name), "Upload " + name) as data:
        data.settimeout(network["stall_timeout_seconds"])
        while block := source.read(64 * 1024):
            view = memoryview(block)
            while view:
                count = data.send(view)
                if not count:
                    raise PushError("Upload connection stopped accepting bytes")
                watch.advance(count)
                view = view[count:]
        if isinstance(data, ssl.SSLSocket):
            # Complete TLS close_notify before reading the FTP completion reply.
            data.unwrap().close()
        if watch.aborted:
            raise PushError("Upload cancelled after stalled progress")
    ftp.voidresp()


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
