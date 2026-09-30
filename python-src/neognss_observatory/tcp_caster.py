#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded, receive-only byte-stream casting from a POSIX serial port or TCP."""

import asyncio
import json
import logging
import math
import os
import random
import signal
import socket
from collections import deque
from pathlib import Path

import click
import serial

LOG = logging.getLogger(__name__)
CHUNK_BYTES = 16 * 1024
WRITE_HIGH = 64 * 1024
WRITE_LOW = 16 * 1024


def _object(value, path, allowed):
    if not isinstance(value, dict):
        raise ValueError(f"{path} must be an object")
    unknown = value.keys() - allowed
    if unknown:
        raise ValueError(f"Unknown {path} keys: {', '.join(sorted(unknown))}")
    return value


def _number(value, path, *, integer=False, nullable=False, minimum=0, maximum=None):
    if nullable and value is None:
        return None
    if (
        isinstance(value, bool)
        or not isinstance(value, int if integer else (int, float))
        or (isinstance(value, float) and not math.isfinite(value))
    ):
        raise ValueError(f"{path} must be a finite {'integer' if integer else 'number'}")
    if value <= minimum or (maximum is not None and value > maximum):
        raise ValueError(f"{path} must be > {minimum}" + (f" and <= {maximum}" if maximum is not None else ""))
    return value


def _text(value, path):
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{path} must be a nonempty string")
    return value


def load_config(path):
    """Validate the complete configuration before opening any sockets/devices."""
    config = _object(json.loads(path.read_text()), "config", {"source", "server"})
    source = _object(
        config.get("source"),
        "source",
        {
            "type",
            "host",
            "port",
            "connect_timeout_seconds",
            "idle_timeout_seconds",
            "reconnect",
            "device",
            "baudrate",
            "bytesize",
            "parity",
            "stopbits",
            "rtscts",
            "xonxoff",
            "dsrdtr",
            "dtr",
            "rts",
        },
    ).copy()
    kind = source.get("type")
    if not isinstance(kind, str) or kind not in {"tcp", "serial"}:
        raise ValueError("source.type must be tcp or serial")
    tcp_keys = {"host", "port", "connect_timeout_seconds"}
    serial_keys = {"device", "baudrate", "bytesize", "parity", "stopbits", "rtscts", "xonxoff", "dsrdtr", "dtr", "rts"}
    incompatible = source.keys() & (serial_keys if kind == "tcp" else tcp_keys)
    if incompatible:
        raise ValueError(f"Keys not applicable to {kind} source: {', '.join(sorted(incompatible))}")
    source["idle_timeout_seconds"] = _number(source.get("idle_timeout_seconds"), "source.idle_timeout_seconds", nullable=True)
    reconnect = _object(
        source.get("reconnect", {}), "source.reconnect", {"initial_delay_seconds", "max_delay_seconds", "stable_connection_seconds"}
    ).copy()
    for key, default in (("initial_delay_seconds", 1), ("max_delay_seconds", 30), ("stable_connection_seconds", 60)):
        reconnect[key] = _number(reconnect.get(key, default), f"source.reconnect.{key}")
    if reconnect["max_delay_seconds"] < reconnect["initial_delay_seconds"]:
        raise ValueError("source.reconnect.max_delay_seconds must be >= initial_delay_seconds")
    source["reconnect"] = reconnect
    if kind == "tcp":
        source["host"] = _text(source.get("host"), "source.host")
        source["port"] = _number(source.get("port"), "source.port", integer=True, maximum=65535)
        source["connect_timeout_seconds"] = _number(source.get("connect_timeout_seconds", 10), "source.connect_timeout_seconds")
    else:
        source["device"] = _text(source.get("device"), "source.device")
        source["baudrate"] = _number(source.get("baudrate", 115200), "source.baudrate", integer=True)
        source.setdefault("bytesize", 8)
        source.setdefault("parity", "N")
        source.setdefault("stopbits", 1)
        for key, choices in (("bytesize", (5, 6, 7, 8)), ("parity", ("N", "E", "O", "M", "S")), ("stopbits", (1, 1.5, 2))):
            if isinstance(source[key], bool) or source[key] not in choices:
                raise ValueError(f"source.{key} must be one of {choices}")
        for key in ("rtscts", "xonxoff", "dsrdtr", "dtr", "rts"):
            source.setdefault(key, key in {"dtr", "rts"})
            if not isinstance(source[key], bool):
                raise ValueError(f"source.{key} must be boolean")
    server = _object(config.get("server", {}), "server", {"host", "port", "max_clients", "client_buffer_bytes"}).copy()
    server["host"] = _text(server.get("host", "127.0.0.1"), "server.host")
    for key, default in (("port", 3002), ("max_clients", 32), ("client_buffer_bytes", 1024**2)):
        server[key] = _number(server.get(key, default), f"server.{key}", integer=True, maximum=65535 if key == "port" else None)
    return {"source": source, "server": server}


class TcpSource:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer

    async def read(self):
        return await self.reader.read(CHUNK_BYTES)

    def close(self):
        self.writer.transport.abort()


class SerialSource:
    def __init__(self, port):
        self.port = port
        self.loop = asyncio.get_running_loop()
        self.fd = port.fileno()

    async def read(self):
        ready = self.loop.create_future()

        def readable():
            if not ready.done():
                ready.set_result(None)

        self.loop.add_reader(self.fd, readable)
        try:
            await ready
            # The POSIX descriptor is nonblocking; pySerial configured its termios.
            return os.read(self.fd, CHUNK_BYTES)
        except BlockingIOError:
            return None
        finally:
            self.loop.remove_reader(self.fd)

    def close(self):
        self.port.close()


async def open_source(config):
    if config["type"] == "tcp":
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(config["host"], config["port"], limit=CHUNK_BYTES), config["connect_timeout_seconds"]
        )
        try:
            writer.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        except BaseException:
            writer.transport.abort()
            raise
        return TcpSource(reader, writer)
    port = serial.Serial(
        port=None,
        baudrate=config["baudrate"],
        bytesize=config["bytesize"],
        parity=config["parity"],
        stopbits=config["stopbits"],
        timeout=0,
        write_timeout=0,
        rtscts=config["rtscts"],
        xonxoff=config["xonxoff"],
        dsrdtr=config["dsrdtr"],
        exclusive=True,
    )
    try:
        port.dtr = config["dtr"]
        port.rts = config["rts"]
        port.port = config["device"]
        port.open()
        return SerialSource(port)
    except BaseException:
        port.close()
        raise


class Client:
    def __init__(self, caster, reader, writer):
        self.caster = caster
        self.reader = reader
        self.writer = writer
        self.peer = writer.get_extra_info("peername")
        self.pending = deque()
        self.pending_bytes = 0
        self.available = asyncio.Event()
        self.task = None
        self.closed = False
        writer.transport.set_write_buffer_limits(high=WRITE_HIGH, low=WRITE_LOW)
        writer.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, WRITE_HIGH)

    def enqueue(self, data):
        if self.pending_bytes + len(data) > self.caster.config["client_buffer_bytes"]:
            self.disconnect("buffer limit", warning=True)
            return
        self.pending.append(data)
        self.pending_bytes += len(data)
        self.available.set()

    def disconnect(self, reason, *, warning=False):
        if self.closed:
            return
        self.closed = True
        self.caster.clients.discard(self)
        self.pending.clear()
        self.pending_bytes = 0
        self.writer.transport.abort()
        if self.task is not None and self.task is not asyncio.current_task():
            self.task.cancel()
        LOG.log(logging.WARNING if warning else logging.INFO, "Client %s disconnected: %s", self.peer, reason)

    async def send(self):
        while True:
            await self.available.wait()
            data = self.pending.popleft()
            self.pending_bytes -= len(data)
            if not self.pending:
                self.available.clear()
            self.writer.write(data)
            await self.writer.drain()
            # drain() and queue consumption can both complete without yielding.
            await asyncio.sleep(0)

    async def discard_input(self):
        while await self.reader.read(CHUNK_BYTES):
            await asyncio.sleep(0)
        # EOF, including a write-half-close, ends the subscription.

    async def serve(self):
        sender = asyncio.create_task(self.send())
        receiver = asyncio.create_task(self.discard_input())
        try:
            done, _ = await asyncio.wait((sender, receiver), return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (OSError, ConnectionError):
            pass
        finally:
            self.disconnect("connection closed")
            sender.cancel()
            receiver.cancel()
            await asyncio.gather(sender, receiver, return_exceptions=True)


class Caster:
    def __init__(self, config):
        self.config = config
        self.clients = set()
        self.tasks = set()
        self.active = False

    def accept(self, reader, writer):
        if not self.active or len(self.clients) >= self.config["max_clients"]:
            writer.transport.abort()
            LOG.debug("Rejected client %s: source unavailable or client limit", writer.get_extra_info("peername"))
            return
        try:
            client = Client(self, reader, writer)
        except OSError:
            writer.transport.abort()
            LOG.exception("Cannot configure client socket")
            return
        self.clients.add(client)
        client.task = asyncio.create_task(client.serve())
        self.tasks.add(client.task)
        client.task.add_done_callback(self.client_done)
        LOG.info("Client %s connected (%d/%d)", client.peer, len(self.clients), self.config["max_clients"])

    def client_done(self, task):
        self.tasks.discard(task)
        if not task.cancelled() and task.exception() is not None:
            LOG.error("Client task failed", exc_info=task.exception())

    def broadcast(self, data):
        for client in tuple(self.clients):
            client.enqueue(data)

    def disconnect_all(self, reason):
        self.active = False
        for client in tuple(self.clients):
            client.disconnect(reason)

    async def follow_source(self, config):
        loop = asyncio.get_running_loop()
        retry = config["reconnect"]
        delay = retry["initial_delay_seconds"]
        while True:
            source = None
            connected_at = None
            try:
                source = await open_source(config)
                connected_at = loop.time()
                last_data = connected_at
                self.active = True
                LOG.info("Upstream connected")
                while True:
                    timeout = config["idle_timeout_seconds"]
                    if timeout is not None:
                        timeout = max(0, last_data + timeout - loop.time())
                    data = await asyncio.wait_for(source.read(), timeout) if timeout is not None else await source.read()
                    if data == b"":
                        raise EOFError("upstream EOF")
                    if data:
                        last_data = loop.time()
                        self.broadcast(data)
                    await asyncio.sleep(0)
            except (OSError, serial.SerialException, EOFError, TimeoutError) as error:
                LOG.warning("Upstream unavailable: %s", str(error) or type(error).__name__)
            finally:
                self.disconnect_all("upstream disconnected")
                if source is not None:
                    source.close()
            if connected_at is not None and loop.time() - connected_at >= retry["stable_connection_seconds"]:
                delay = retry["initial_delay_seconds"]
            sleep_seconds = random.uniform(0.8 * delay, delay)
            LOG.info("Retrying upstream in %.2f seconds", sleep_seconds)
            await asyncio.sleep(sleep_seconds)
            delay = min(delay * 2, retry["max_delay_seconds"])


async def run(config):
    loop = asyncio.get_running_loop()
    stopping = asyncio.Event()
    installed = []
    caster = Caster(config["server"])
    upstream = None
    server = None
    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(signum, stopping.set)
            installed.append(signum)
        server = await asyncio.start_server(caster.accept, config["server"]["host"], config["server"]["port"], limit=CHUNK_BYTES)
        LOG.info("Listening on %s", ", ".join(str(sock.getsockname()) for sock in server.sockets))
        upstream = asyncio.create_task(caster.follow_source(config["source"]))
        stopped = asyncio.create_task(stopping.wait())
        try:
            done, _ = await asyncio.wait((upstream, stopped), return_when=asyncio.FIRST_COMPLETED)
            if upstream in done:
                upstream.result()
        finally:
            stopped.cancel()
            await asyncio.gather(stopped, return_exceptions=True)
    finally:
        if server is not None:
            server.close()
        caster.disconnect_all("server stopping")
        if upstream is not None:
            upstream.cancel()
            await asyncio.gather(upstream, return_exceptions=True)
        await asyncio.gather(*caster.tasks, return_exceptions=True)
        if server is not None:
            await server.wait_closed()
        for signum in installed:
            loop.remove_signal_handler(signum)
        LOG.info("Caster stopped")


@click.command()
@click.option(
    "--config",
    "config_path",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    required=True,
    help="JSON source and server configuration.",
)
@click.option(
    "--log-level", type=click.Choice(["DEBUG", "INFO", "WARNING", "ERROR"], case_sensitive=False), default="INFO", show_default=True
)
def cli(config_path, log_level):
    """Cast one serial or TCP upstream to independent receive-only TCP clients."""
    logging.basicConfig(level=log_level.upper(), format="%(levelname)s %(message)s")
    try:
        config = load_config(config_path)
        asyncio.run(run(config))
    except (OSError, ValueError, RuntimeError, serial.SerialException) as error:
        raise click.ClickException(str(error)) from error


if __name__ == "__main__":
    cli()
