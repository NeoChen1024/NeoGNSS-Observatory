#!/usr/bin/env python3
"""CLI, output lifecycle, transport recovery, and variant dispatch regressions."""

import os
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

LOGGER = Path(os.environ["CPPUBX2_LOGGER"])


def frame(cls, msg, payload=b""):
    data = bytes((cls, msg)) + struct.pack("<H", len(payload)) + payload
    a = b = 0
    for byte in data:
        a = (a + byte) & 255
        b = (b + a) & 255
    return b"\xb5\x62" + data + bytes((a, b))


def pvt(year=2026, month=9, day=5, tow=1000, valid=3):
    data = bytearray(92)
    struct.pack_into("<IH", data, 0, tow, year)
    data[6:12] = bytes((month, day, 0, 0, 0, valid))
    data[20:22] = bytes((3, 1))
    return frame(1, 7, data)


def epoch(week=2434, tow=518400000, valid=3, eoe_tow=None):
    return frame(1, 0x20, struct.pack("<IihbBI", tow, 0, week, 18, valid, 0)) + frame(
        1, 0x61, struct.pack("<I", tow if eoe_tow is None else eoe_tow)
    )


class LoggerTests(unittest.TestCase):
    def run_logger(self, data=b"", args=("-n", "-q"), cwd=None):
        return subprocess.run([str(LOGGER), *args], input=data, cwd=cwd, capture_output=True, timeout=10)

    def test_cli_port_errors(self):
        for port in ("abc", "0", "-1", "65536", "123x", "999999999999", "+123", " 123"):
            with self.subTest(port=port):
                result = self.run_logger(args=("-n", "-t", "localhost:" + port))
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"Invalid TCP port", result.stderr)
        result = self.run_logger(args=("-n", "unexpected"))
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"Unexpected argument", result.stderr)
        result = self.run_logger(args=("-f", "/does/not/exist", "-t", "localhost:1"))
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"mutually exclusive", result.stderr)

    def test_eof_at_every_frame_offset(self):
        data = frame(5, 1, b"\x01\x02")
        self.assertEqual(self.run_logger().returncode, 0)
        self.assertEqual(self.run_logger(data).returncode, 0)
        for offset in range(1, len(data)):
            with self.subTest(offset=offset):
                result = self.run_logger(data[:offset])
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"Truncated UBX frame", result.stderr)

    def test_recording_rotates_complete_gpst_epoch_at_eoe(self):
        with tempfile.TemporaryDirectory() as directory:
            from datetime import datetime, timedelta

            packets = [pvt(valid=0) + epoch(tow=t) for t in (518399000, 518400000)]
            result = self.run_logger(b"".join(packets), ("-q",), directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            for packet, tow in zip(packets, (518399, 518400)):
                date = datetime(1980, 1, 6) + timedelta(seconds=2434 * 604800 + tow)
                path = Path(directory) / date.strftime("%Y-%m/GPST-%Y-%m-%d--%H-%M-%S.ubx")
                self.assertEqual(path.read_bytes(), packet)

    def test_each_eoe_requires_fresh_matching_valid_timegps(self):
        cases = [
            frame(1, 0x61, struct.pack("<I", 0)),
            epoch() + frame(1, 0x61, struct.pack("<I", 518401000)),
            epoch(valid=0),
            epoch(eoe_tow=1),
            epoch() + epoch(),
        ]
        for data in cases:
            with self.subTest(data=data), tempfile.TemporaryDirectory() as directory:
                self.assertNotEqual(self.run_logger(data, ("-q",), directory).returncode, 0)

    def test_incomplete_epoch_is_not_published(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_logger(epoch()[:-12], ("-q",), directory)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(list(Path(directory).rglob("*.ubx")))

    def test_gps_week_rollover(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_logger(epoch(tow=604799000) + epoch(week=2435, tow=0), ("-q",), directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(list(Path(directory).rglob("*.ubx"))), 2)

    def test_buffered_output_failure_at_eof_and_rotation(self):
        for rotate in (False, True):
            with self.subTest(rotate=rotate), tempfile.TemporaryDirectory() as directory:
                from datetime import datetime, timedelta

                date = datetime(1980, 1, 6) + timedelta(seconds=2434 * 604800 + 518400)
                folder = Path(directory) / date.strftime("%Y-%m")
                folder.mkdir()
                (folder / date.strftime("GPST-%Y-%m-%d--%H-%M-%S.ubx")).symlink_to("/dev/full")
                data = epoch() + (epoch(week=2435, tow=0) if rotate else b"")
                result = self.run_logger(data, ("-q",), directory)
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"UBX output close", result.stderr)

    def test_tcp_timeout_discards_partial_frame_and_resynchronizes(self):
        with socket.socket() as server:
            server.bind(("127.0.0.1", 0))
            server.listen()
            server.settimeout(5)
            endpoint = f"127.0.0.1:{server.getsockname()[1]}"
            process = subprocess.Popen([str(LOGGER), "-n", "-d", "-t", endpoint], stderr=subprocess.PIPE)
            try:
                with server.accept()[0] as connection:
                    packet = frame(5, 1, b"\x01\x02")
                    connection.sendall(packet[:7])
                    time.sleep(6)
                    connection.sendall(packet[7:] + packet)
                    time.sleep(0.2)
                process.terminate()
                _, output = process.communicate(timeout=3)
                self.assertIn(b"read timeout", output)
                self.assertIn(b"resynchronizing", output)
                self.assertEqual(output.count(b"(ACK-ACK, clsID=1, msgID=2)"), 1)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_variant_dispatch(self):
        cases = [
            (6, 0x17, 4, 0, 0, "CFG-NMEAvX"),
            (6, 0x17, 12, 0, 0, "CFG-NMEAv0"),
            (6, 0x17, 20, 0, 0, "CFG-NMEA"),
            (1, 0x60, 20, 0, 0, "NAV-AOPSTATUS-L"),
            (1, 0x60, 16, 0, 0, "NAV-AOPSTATUS"),
            (1, 0x45, 64, 0, 1, "NAV-DAHEADINGHP"),
            (1, 0x45, 60, 0, 2, "NAV-DAHEADING"),
            (1, 0x3C, 40, 0, 0, "NAV-RELPOSNED-V0"),
            (1, 0x3C, 64, 0, 1, "NAV-RELPOSNED"),
            (2, 0x59, 16, 1, 1, "RXM-RLM-S"),
            (2, 0x59, 28, 1, 2, "RXM-RLM-L"),
            (2, 0x72, 528, 0, 0, "RXM-PMP-V0"),
            (2, 0x72, 24, 0, 1, "RXM-PMP-V1"),
            (0x27, 9, 12, 0, 1, "SEC-SIG-V1"),
            (0x27, 9, 4, 0, 2, "SEC-SIG-V2"),
            (0x27, 3, 9, 0, 1, "SEC-UNIQID"),
            (0x27, 3, 10, 0, 2, "SEC-UNIQID-V2"),
        ]
        for cls, msg, length, offset, value, name in cases:
            with self.subTest(name=name):
                payload = bytearray(length)
                payload[offset] = value
                result = self.run_logger(frame(cls, msg, payload), ("-n", "-d"))
                self.assertEqual(result.returncode, 0)
                self.assertIn(f"({name},".encode(), result.stderr)
                self.assertNotIn(b"exceeds", result.stderr)
        result = self.run_logger(frame(0x27, 9, b"\xff\x00\x00\x00"), ("-n", "-d"))
        self.assertIn(b"UBX-SEC-SIG (4)", result.stderr)
        self.assertNotIn(b"(SEC-SIG-V", result.stderr)
