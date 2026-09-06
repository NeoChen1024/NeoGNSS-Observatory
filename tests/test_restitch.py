"""Synthetic reconstruction and corruption regression tests."""

import os
import struct
import subprocess
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from neognss_observatory.ubx_output import publish
from neognss_observatory.ubx_reconstruction import build_plan, segment_plan
from neognss_observatory.ubx_restitch import (
    EOE,
    NO_NAV,
    NOISE,
    PARTIAL,
    UNKNOWN,
    epochs,
    inventory_source,
    sha256,
)

INDEXER = Path(
    os.environ.get("CPPUBX2_ARCHIVE_INDEX", Path(__file__).resolve().parents[1] / "build/libcppubx2/cppubx2_archive_index")
)


def frame(cls, msg, payload=b""):
    body = bytes([cls, msg]) + struct.pack("<H", len(payload)) + payload
    a = b = 0
    for x in body:
        a = (a + x) & 255
        b = (b + a) & 255
    return b"\xb5\x62" + body + bytes([a, b])


def second(gpst, tow, *, nav=True, eoe=True, offset=0.01, marker=0):
    week, tow = divmod(gpst, 604800)
    rawx = bytearray(16)
    struct.pack_into("<dH", rawx, 0, tow + offset, week)
    rawx[10] = 18
    rawx[12] = 1
    data = frame(2, 0x13, bytes([marker]) * 8) + frame(2, 0x15, rawx)
    if nav:
        date = datetime.fromtimestamp(gpst + 315964800 - 18, timezone.utc)
        pvt = bytearray(92)
        struct.pack_into("<IHBBBBBB", pvt, 0, tow * 1000, date.year, date.month, date.day, date.hour, date.minute, date.second, 3)
        data += frame(1, 0x20, struct.pack("<IihbBI", tow * 1000, 0, week, 18, 3, 0))
        data += frame(1, 7, pvt)
    if eoe:
        data += frame(1, 0x61, struct.pack("<I", tow * 1000))
    return data


@unittest.skipUnless(INDEXER.exists(), "Build cppubx2_archive_index first")
class ArchiveScanTests(unittest.TestCase):
    def test_pvt_calendar_is_not_a_gpst_anchor(self):
        pvt = bytearray(92)
        struct.pack_into("<IHBBBBBB", pvt, 0, 1000, 2025, 4, 1, 0, 0, 0, 3)
        rows = self.scan(frame(1, 7, pvt) + frame(1, 0x61, struct.pack("<I", 1000)))
        self.assertEqual(rows[0].gpst, UNKNOWN)

    def scan(self, data):
        with tempfile.TemporaryDirectory() as directory:
            source, index = Path(directory) / "input.ubx", Path(directory) / "epochs.idx"
            source.write_bytes(data)
            subprocess.run([str(INDEXER), str(source), str(index)], check=True)
            result = list(epochs(index))
        self.assertEqual(result[0].begin, 0)
        self.assertEqual(result[-1].end, len(data))
        self.assertTrue(all(a.end == b.begin for a, b in zip(result, result[1:])))
        return result

    def test_rawx_clock_steering_and_order(self):
        start = 1368835200
        data = b"gpsd header\r\n" + b"".join(second(start + i, 172818 + i, offset=-0.01) for i in range(3))
        rows = self.scan(data)
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0].flags, NOISE)
        self.assertEqual([r.gpst for r in rows[1:]], [start, start + 1, start + 2])
        self.assertTrue(all(r.flags & EOE and not r.flags & PARTIAL for r in rows[1:]))
        self.assertEqual(rows[1].week, 2263)

    def test_corrupt_length_and_checksum_resynchronize(self):
        valid = second(1368835200, 172818)
        rows = self.scan(b"\xb5\x62\x01\x07\xff\xffbroken" + valid + b"\xb5\x62")
        self.assertEqual(rows[0].flags, NOISE)
        self.assertEqual(rows[1].gpst, 1368835200)
        self.assertEqual(rows[-1].flags, NOISE)

    def test_missing_nav_and_eoe_preserves_bytes(self):
        data = second(1368835200, 172818)
        data += second(1368835201, 172819, nav=False, eoe=False)
        data += second(1368835202, 172820)
        rows = self.scan(data)
        self.assertEqual(len(rows), 3)
        self.assertTrue(rows[1].flags & NO_NAV)
        self.assertEqual(rows[1].gpst, 1368835201)
        self.assertEqual(rows[2].gpst, 1368835202)

    def test_untimed_tail_and_week_rollover(self):
        data = second(1368662381, 604799) + second(1368662382, 0)
        data += frame(2, 0x13, b"asynchronous")
        rows = self.scan(data)
        self.assertEqual(rows[1].gpst, 1368662382)
        self.assertEqual(rows[-1].gpst, UNKNOWN)
        self.assertTrue(rows[-1].flags & PARTIAL)


@unittest.skipUnless(INDEXER.exists(), "Build cppubx2_archive_index first")
class ReconstructionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = self.root / "state"
        self.state.mkdir()
        self.tool_sha = sha256(INDEXER)

    def source(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return inventory_source(path, self.state, INDEXER, self.tool_sha)

    def materialize(self, artifact):
        return b"".join(Path(s["source"]).read_bytes()[s["begin"] : s["end"]] for s in artifact["spans"])

    def test_exact_overlap_preserves_async_packets(self):
        packets = [second(1368835200 + i, 172818 + i, marker=i) for i in range(9)]
        # Incoming first epoch lacks its first complete asynchronous frame.
        first = self.source("a.ubx", b"gpsd\n" + b"".join(packets[:6]))
        last = self.source("b.ubx", b"gpsd\n" + packets[3][16:] + b"".join(packets[4:]))
        plan = segment_plan(build_plan([first, last]))
        self.assertEqual(len(plan["joins"]), 1)
        self.assertEqual(len(plan["artifacts"]), 1)
        self.assertEqual(self.materialize(plan["artifacts"][0]), b"".join(packets))

    def test_split_epoch_at_file_boundary_retains_all_bytes(self):
        packets = [second(1368835200 + i, 172818 + i, marker=i) for i in range(3)]
        # 16-byte SFRBX plus 24-byte RAWX precede NAV-PVT and NAV-EOE.
        first = self.source("a.ubx", packets[0] + packets[1][:40])
        last = self.source("b.ubx", packets[1][40:] + packets[2])
        plan = segment_plan(build_plan([first, last]))
        self.assertEqual(plan["joins"][0]["kind"], "split_epoch_continuation")
        self.assertEqual(self.materialize(plan["artifacts"][0]), b"".join(packets))
        self.assertEqual(plan["byte_accounting"]["verified_duplicate_bytes"], 0)

    def test_complete_same_epoch_is_not_a_split_continuation(self):
        packets = [second(1368835200 + i, 172818 + i) for i in range(3)]
        first = self.source("a.ubx", packets[0] + packets[1])
        last = self.source("b.ubx", packets[1][40:] + packets[2])
        with self.assertRaisesRegex(ValueError, "Unverified overlap"):
            build_plan([first, last])

    def test_overlap_with_changed_payload_fails_closed(self):
        packets = [second(1368835200 + i, 172818 + i, marker=i) for i in range(9)]
        first = self.source("a.ubx", b"".join(packets[:6]))
        packets[3] = second(1368835203, 172821, marker=99)
        last = self.source("b.ubx", b"".join(packets[3:]))
        with self.assertRaisesRegex(ValueError, "Unverified overlap"):
            build_plan([first, last])

    def test_gpst_midnight_gap_and_no_nav_intervals(self):
        start = int(datetime(2023, 5, 23, 23, 59, 59, tzinfo=timezone.utc).timestamp()) - 315964800
        packets = [
            second(start, 259217),
            second(start + 1, 259218),
            second(start + 2, 259219, nav=False, eoe=False),
            second(start + 3, 259220),
            second(start + 7, 259224),
        ]
        plan = segment_plan(build_plan([self.source("a.ubx", b"".join(packets))]))
        main = [a for a in plan["artifacts"] if a["kind"] == "gpst_segment"]
        self.assertEqual(
            [a["name"] for a in main],
            [
                "GPST-2023-05-23--23-59-59.ubx",
                "GPST-2023-05-24--00-00-00.ubx",
                "GPST-2023-05-24--00-00-02.ubx",
                "GPST-2023-05-24--00-00-06.ubx",
            ],
        )
        unassigned = [a for a in plan["artifacts"] if a["kind"] == "unassigned_frames"]
        # Without EOE, the following untimed SFRBX cannot safely be assigned to
        # the next epoch before its RAWX timestamp arrives. Preserve it too.
        self.assertEqual(self.materialize(unassigned[0]), packets[2] + packets[3][:16])
        self.assertEqual(sum(a["size"] for a in plan["artifacts"]), sum(map(len, packets)))

    def test_publication_readback_resume_and_existing_output(self):
        data = second(1368835200, 172818)
        plan = segment_plan(build_plan([self.source("a.ubx", data)]))
        output = self.root / "output"
        result = publish(plan, output, INDEXER)
        self.assertEqual(result["bytes"], len(data))
        self.assertEqual(publish(plan, output, INDEXER), result)
        final = output / plan["artifacts"][0]["name"]
        final.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "failed verification"):
            publish(plan, output, INDEXER)
        another = self.root / "occupied"
        another.mkdir()
        (another / "keep").write_bytes(b"existing")
        with self.assertRaisesRegex(ValueError, "not empty"):
            publish(plan, another, INDEXER)
        self.assertEqual((another / "keep").read_bytes(), b"existing")
