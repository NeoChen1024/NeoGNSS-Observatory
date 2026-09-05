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

    def test_recording_uses_pvt_without_eoe_and_full_date(self):
        with tempfile.TemporaryDirectory() as directory:
            dates = [(2026, 9, 5), (2026, 10, 5), (2027, 10, 5)]
            packets = [pvt(*date) for date in dates]
            mismatch = frame(1, 0x61, struct.pack("<I", 2000))
            result = self.run_logger(b"".join(packets) + mismatch, ("-q",), directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"EOE iTOW mismatch", result.stderr)
            for index, (year, month, day) in enumerate(dates):
                path = Path(directory) / f"{year:04}-{month:02}" / f"{year:04}{month:02}{day:02}T000000.ubx"
                expected = packets[index] + (mismatch if index == 2 else b"")
                self.assertEqual(path.read_bytes(), expected)

    def test_invalid_pvt_does_not_change_output_date(self):
        with tempfile.TemporaryDirectory() as directory:
            valid = pvt()
            invalid = pvt(month=10, valid=0)
            result = self.run_logger(valid + invalid, ("-q",), directory)
            self.assertEqual(result.returncode, 0)
            outputs = list(Path(directory).rglob("*.ubx"))
            self.assertEqual(len(outputs), 1)
            self.assertEqual(outputs[0].read_bytes(), valid + invalid)

    def test_buffered_output_failure_at_eof_and_rotation(self):
        for rotate in (False, True):
            with self.subTest(rotate=rotate), tempfile.TemporaryDirectory() as directory:
                folder = Path(directory) / "2026-09"
                folder.mkdir()
                (folder / "20260905T000000.ubx").symlink_to("/dev/full")
                data = pvt() + (pvt(month=10) if rotate else b"")
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
