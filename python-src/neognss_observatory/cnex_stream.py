#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""CommonNEX batch delivery and bounded live input; independent of persistence."""

import json
import math
import queue
import socket
import struct
import threading
import time
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType

import click
import pyarrow as pa

from . import _native

CATALOGS = ("observations", "events", "raw-bits", "receiver-telemetry")
MAX_MESSAGE = 16 * 1024**2


@dataclass(frozen=True)
class CnexBatchGroup:
    sequence: int
    catalogs: Mapping[str, tuple[pa.RecordBatch, ...]]
    notice: str | None = None

    def __post_init__(self):
        if self.sequence < 0 or any(k not in CATALOGS for k in self.catalogs):
            raise ValueError("Invalid CommonNEX batch group")
        object.__setattr__(self, "catalogs", MappingProxyType({k: tuple(v) for k, v in self.catalogs.items()}))

    @property
    def nbytes(self):
        return sum(b.nbytes for batches in self.catalogs.values() for b in batches)

    def ordered_batches(self):
        """The stable catalog order used by the archive writer."""
        return tuple(b for name in CATALOGS for b in self.catalogs.get(name, ()))


def batch_group(sequence, native_batches, *, include_empty=False):
    batches = tuple(pa.record_batch(b) for b in native_batches)
    if len(batches) != len(CATALOGS):
        raise ValueError("Unexpected native catalog count")
    return CnexBatchGroup(sequence, {k: (b,) for k, b in zip(CATALOGS, batches) if include_empty or b.num_rows})


class CnexStream:
    """Single-owner producer. Drain never finishes a pending measurement epoch."""

    def __init__(self, protocol, setup_id, period_seconds=1, period_ps=0, antenna=0, decode_workers=1, max_bytes=4 * 1024**2):
        if max_bytes <= 0:
            raise ValueError("max_bytes must be positive")
        self.args = (protocol, setup_id, antenna, period_seconds, period_ps, decode_workers)
        self.reader = _native.CnexObservationReader(*self.args)
        self.max_bytes = max_bytes
        self.sequence = 0
        self.pending = {}
        self.pending_bytes = 0
        self.closed = False
        self.reported_partial_rows = 0

    def feed(self, data):
        if self.closed:
            raise RuntimeError("Stream is closed")
        # The caller must drain regularly; never build an unbounded output queue.
        if self.pending_bytes >= self.max_bytes:
            raise BufferError("Drain CommonNEX batches before feeding more input")
        if len(data) > 64 * 1024:
            raise ValueError("Live feed chunks must not exceed 64 KiB")
        try:
            group = batch_group(self.sequence, self.reader.feed(data))
        except Exception:
            self.closed = True
            raise
        for catalog, batches in group.catalogs.items():
            self.pending.setdefault(catalog, []).extend(batches)
        self.pending_bytes += group.nbytes

    def drain(self, notice=None):
        partial = self.reader.summary()["telemetry_partial_rows"]
        if partial > self.reported_partial_rows:
            warning = f"partial telemetry windows: {partial - self.reported_partial_rows}"
            notice = f"{notice}; {warning}" if notice else warning
        self.reported_partial_rows = partial
        if not self.pending and notice is None:
            return None
        result = CnexBatchGroup(self.sequence, self.pending, notice)
        self.sequence += 1
        self.pending = {}
        self.pending_bytes = 0
        return result

    def discontinuity(self, reason):
        """Return preceding records with a boundary notice, then reset context.

        Consumers apply this notice AFTER the group's records, never as reboot.
        The next receiver segment has fresh framing and time/epoch association.
        """
        if self.closed:
            raise RuntimeError("Stream is closed")
        self._finish_telemetry()
        result = self.drain(f"discontinuity: {reason}; pending={json.dumps(self.tail_status(), sort_keys=True)}")
        self.reader = _native.CnexObservationReader(*self.args)
        self.reported_partial_rows = 0
        return result

    def tail_status(self):
        s = self.reader.summary()
        return {k: s[k] for k in ("pending_epoch", "pending_frame_bytes", "measextra_pending")}

    def finish(self):
        if self.closed:
            raise RuntimeError("Stream is closed")
        self._finish_telemetry()
        self.closed = True
        # Report, rather than invent completeness for, the remaining raw tail.
        return self.drain(f"end: pending={json.dumps(self.tail_status(), sort_keys=True)}")

    def _finish_telemetry(self):
        group = batch_group(self.sequence, self.reader.finish_telemetry())
        for name, batches in group.catalogs.items():
            self.pending.setdefault(name, []).extend(batches)
        self.pending_bytes += group.nbytes


def encode_group(group):
    """Self-contained IPC per catalog; framing version is not a CommonNEX schema."""
    if group.nbytes > MAX_MESSAGE:
        raise BufferError("CommonNEX batch group exceeds 16 MiB")
    payloads = []
    entries = []
    for catalog in CATALOGS:
        batches = group.catalogs.get(catalog, ())
        if not batches:
            continue
        sink = pa.BufferOutputStream()
        with pa.ipc.new_stream(sink, batches[0].schema) as writer:
            for batch in batches:
                writer.write_batch(batch)
        payload = sink.getvalue()
        entries.append([catalog, payload.size])
        payloads.append(payload)
    header = json.dumps(dict(version=1, sequence=group.sequence, notice=group.notice, catalogs=entries)).encode()
    size = 4 + len(header) + sum(p.size for p in payloads)
    if size > MAX_MESSAGE:
        raise BufferError("CommonNEX IPC message exceeds 16 MiB")
    return struct.pack("!I", len(header)) + header + b"".join(memoryview(p) for p in payloads)


def decode_group(data):
    if not 4 <= len(data) <= MAX_MESSAGE:
        raise ValueError("Invalid CommonNEX IPC message size")
    header_size = struct.unpack_from("!I", data)[0]
    offset = 4 + header_size
    if offset > len(data):
        raise ValueError("Truncated CommonNEX IPC header")
    header = json.loads(data[4:offset])
    if header["version"] != 1:
        raise ValueError("Unsupported CommonNEX transport version")
    catalogs = {}
    for catalog, size in header["catalogs"]:
        if catalog not in CATALOGS or catalog in catalogs or not isinstance(size, int) or size <= 0 or size > len(data) - offset:
            raise ValueError("Invalid CommonNEX catalog payload")
        # BufferReader retains the backing bytes for all resulting batches.
        with pa.ipc.open_stream(pa.BufferReader(memoryview(data)[offset : offset + size])) as reader:
            catalogs[catalog] = tuple(reader)
        for batch in catalogs[catalog]:
            batch.validate(full=True)
        offset += size
    if offset != len(data):
        raise ValueError("Trailing CommonNEX IPC bytes")
    return CnexBatchGroup(header["sequence"], catalogs, header["notice"])


def send_group(connection, group):
    """Dedicated multiprocessing Connection writer; no pickle data path."""
    connection.send_bytes(encode_group(group))


def receive_group(connection):
    return decode_group(connection.recv_bytes(MAX_MESSAGE))


def write_group(target, group):
    """Length-framed group for a blocking binary file or socket.makefile()."""
    data = encode_group(group)
    packet = memoryview(struct.pack("!I", len(data)) + data)
    while packet:
        written = target.write(packet)
        if written is None or written <= 0:
            raise OSError("Incomplete CommonNEX IPC write")
        packet = packet[written:]
    target.flush()


def read_group(source):
    """Return None only at a clean group boundary; truncated messages raise."""

    def exact(length, allow_eof=False):
        parts = bytearray()
        while len(parts) < length:
            chunk = source.read(length - len(parts))
            if not chunk:
                if allow_eof and not parts:
                    return None
                raise EOFError("Truncated CommonNEX IPC message")
            parts.extend(chunk)
        return bytes(parts)

    prefix = exact(4, allow_eof=True)
    if prefix is None:
        return None
    size = struct.unpack("!I", prefix)[0]
    if not 4 <= size <= MAX_MESSAGE:
        raise ValueError("Invalid CommonNEX IPC frame size")
    return decode_group(exact(size))


def tcp_groups(host, port, stream, *, max_latency=5.0, duration=None, buffer_bytes=4 * 1024**2):
    """Yield groups from a single connection. EOF/errors are explicit, not reconnects.

    A bounded acquisition thread isolates socket reads from processing/I/O stalls.
    Capacity counts input payloads (plus at most one active 64 KiB receive).
    Host monotonic time controls delivery only, never scientific timestamps.
    """
    if not math.isfinite(max_latency) or max_latency <= 0 or buffer_bytes < 65536:
        raise ValueError("Invalid live latency or input buffer size")
    if duration is not None and (not math.isfinite(duration) or duration <= 0):
        raise ValueError("duration must be positive")
    received = queue.Queue(maxsize=buffer_bytes // 65536)
    stop = threading.Event()
    errors = []
    done = threading.Event()
    started = time.monotonic()

    with socket.create_connection((host, port), timeout=10) as sock:
        sock.settimeout(0.5)

        def acquire():
            try:
                while not stop.is_set():
                    if duration is not None and time.monotonic() - started >= duration:
                        break
                    try:
                        data = sock.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        raise EOFError("Receiver TCP stream closed")
                    try:
                        received.put_nowait(data)
                    except queue.Full as error:
                        raise BufferError("Live input buffer full; acquisition stopped") from error
            except Exception as error:
                errors.append(error)
            finally:
                done.set()

        worker = threading.Thread(target=acquire, name="cnex-tcp", daemon=True)
        worker.start()
        deadline = time.monotonic() + max_latency
        try:
            while not done.is_set() or not received.empty():
                try:
                    data = received.get(timeout=max(0.001, min(0.5, deadline - time.monotonic())))
                except queue.Empty:
                    data = None
                if data is not None:
                    stream.feed(data)
                if time.monotonic() >= deadline or stream.pending_bytes >= stream.max_bytes:
                    group = stream.drain()
                    if group is not None:
                        yield group
                    deadline = time.monotonic() + max_latency
            if errors:
                yield stream.discontinuity(str(errors[0]))
                raise errors[0]
            yield stream.finish()
        finally:
            stop.set()
            worker.join()


@click.command()
@click.option("--host", required=True, help="Receiver TCP hostname or unbracketed IPv6 address.")
@click.option("--port", type=click.IntRange(1, 65535), default=2006, show_default=True)
@click.option("-p", "--protocol", type=click.Choice(["ubx", "sbf"]), required=True)
@click.option("--setup", "setup_path", type=click.Path(exists=True, path_type=Path), required=True)
@click.option("--max-latency", type=click.FloatRange(min=0, min_open=True), default=5.0, show_default=True)
@click.option(
    "--duration", type=click.FloatRange(min=0, min_open=True), help="Stop after this many seconds; otherwise run until interrupted."
)
@click.option("--output", type=click.Path(path_type=Path), help="Exclusive-create framed CommonNEX IPC output, not ParquetNEX.")
def cli(host, port, protocol, setup_path, max_latency, duration, output):
    """Normalize a live receiver stream; print machine-readable batch summaries."""
    from contextlib import nullcontext

    from .setup_metadata import validate_setup

    try:
        setup = validate_setup(json.loads(setup_path.read_text()))
        seconds, fraction = setup["epoch_period_s"].split(".")
        stream = CnexStream(protocol, setup["setup_id"], int(seconds), int(fraction))
        with output.open("xb") if output else nullcontext() as target:
            for group in tcp_groups(host, port, stream, max_latency=max_latency, duration=duration):
                if target:
                    write_group(target, group)
                click.echo(
                    json.dumps(
                        dict(
                            sequence=group.sequence,
                            rows={k: sum(b.num_rows for b in group.catalogs.get(k, ())) for k in CATALOGS},
                            notice=group.notice,
                        )
                    )
                )
    except (OSError, ValueError, RuntimeError, BufferError, EOFError, OverflowError) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
