# SPDX-License-Identifier: GPL-3.0-only
"""Offline protocol and persistence regression tests; no Earthdata account needed."""

import gzip
import hashlib
import io
import json
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

import requests
from click.testing import CliRunner
from rich.console import Console
from rich.progress import Progress

from neognss_observatory.cddis_download import cli
from neognss_observatory.download_common import (
    AuthError,
    DownloadError,
    atomic_json,
    json_digest,
    local_path,
    root_lock,
)
from neognss_observatory.download_http import (
    ARCHIVE,
    DEFAULT_NETWORK,
    HTTP,
    retry_delay,
)
from neognss_observatory.download_inventory_cache import InventoryCache
from neognss_observatory.download_plan import (
    Inventory,
    attach_checksums,
    load_plan,
    make_plan,
    parse_listing,
    read_config,
    summarize,
    upstream_evidence_status,
)
from neognss_observatory.download_plan_io import plan_digest, write_plan
from neognss_observatory.download_transfer import (
    download_one,
    fetch,
    read_records,
    validate,
    verify,
)


def product():
    return {
        "id": "orbit",
        "layout": "week",
        "base": "gnss/products",
        "pattern": "COD0MGXFIN_{yyyy}{doy}0000_01D_05M_ORB.SP3.gz",
        "required": True,
        "format": "sp3",
    }


def item(payload, name="test.sp3.gz"):
    return {
        "path": "cddis/archive/gnss/products/2361/" + name,
        "url": ARCHIVE + "gnss/products/2361/" + name,
        "size": len(payload),
        "required": True,
        "product": "orbit",
        "format": "sp3",
        "upstream_status": "unavailable",
        "nominal_dates": ["2025-04-06"],
    }


class Response:
    def __init__(self, body=b"", code=200, headers=None, broken=False):
        self.body = body
        self.status_code = code
        self.headers = headers or {}
        self.broken = broken

    def iter_content(self, _):
        if self.body:
            yield self.body
        if self.broken:
            raise requests.ConnectionError("simulated connection loss")

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class FakeHTTP:
    def __init__(self, responses=(), pages=None):
        self.responses = iter(responses)
        self.calls = []
        self.pages = pages or {}
        self.settings = {**DEFAULT_NETWORK, "retries": 1}
        self.stopped = threading.Event()

    def request(self, url, headers=None):
        self.calls.append((url, headers or {}))
        return next(self.responses)

    def text(self, url):
        self.calls.append((url, {}))
        return self.pages.get(url)

    def backoff(self, *_):
        pass


class DownloaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.payload = gzip.compress(b"#cP2025 04 06 00 00 00\n" + b"P example\n" * 100)
        self.item = item(self.payload)

    def tearDown(self):
        self.temp.cleanup()

    def test_listing_count_detects_truncation_and_html(self):
        self.assertEqual(parse_listing("a.gz 123\n# Total number of files = 1\n#WARNING: limit\n"), {"a.gz": 123})
        for text in ("a.gz 123\n# Total number of files = 2", "a.gz 123", "<html>login</html>"):
            with self.assertRaises(DownloadError):
                parse_listing(text)

    def test_config_and_cli_help(self):
        config, _ = read_config("config/products.example.toml")
        self.assertEqual(len(config["products"]), 7)
        for command in ([], ["plan"], ["fetch"], ["status"], ["verify"]):
            result = CliRunner().invoke(cli, command + ["--help"])
            self.assertEqual(result.exit_code, 0, result.output)
        result = CliRunner().invoke(
            cli, ["plan", "--config", "config/products.example.toml", "--start", "bad", "--output", "unused.json"]
        )
        self.assertEqual(result.exit_code, 2)

    def test_latest_frontier_uses_listings_not_checksum(self):
        name = "COD0MGXFIN_20250960000_01D_05M_ORB.SP3.gz"
        pages = {
            ARCHIVE + "gnss/products/": '<a title="DataDirectory" href="2361">2361</a>',
            ARCHIVE + "gnss/products/2361/*?list": f"{name} 100\n# Total number of files = 1\n",
        }
        config = {"start": "2025-04-06", "guard_days": 0, "products": [product()], "network": DEFAULT_NETWORK}
        plan = make_plan(config, "example", FakeHTTP(pages=pages))
        self.assertEqual(plan["end"], "2025-04-06")
        self.assertEqual(len(plan["slots"]), 1)
        self.assertEqual(plan["files"][0]["coverage_status"], "unverified")

    def test_gaps_do_not_stop_inventory(self):
        name = "COD0MGXFIN_20251030000_01D_05M_ORB.SP3.gz"
        pages = {ARCHIVE + "gnss/products/2362/*?list": f"{name} 100\n# Total number of files = 1\n"}
        config = {"start": "2025-04-06", "end": "2025-04-13", "guard_days": 0, "products": [product()], "network": DEFAULT_NETWORK}
        plan = make_plan(config, "example", FakeHTTP(pages=pages))
        self.assertEqual(len(plan["files"]), 1)
        self.assertEqual(sum("issue" in slot for slot in plan["slots"]), 7)

    def test_checksum_scope_missing_conflicts_and_snapshot_identity(self):
        source = self.root / "SHA512SUMS"
        raw = ("a" * 128 + "  test.sp3.gz\n" + "b" * 128 + "  test.sp3.gz\n" + "invalid row\n").encode()
        source.write_bytes(raw)
        outside = {**self.item, "url": ARCHIVE + "gnss/data/test.sp3.gz", "upstream_status": "unavailable"}
        plan = {"files": [dict(self.item), outside]}
        attach_checksums(plan, {"local_file": str(source), "source_url": ARCHIVE + "gnss/products/SHA512SUMS"})
        self.assertEqual(plan["files"][0]["upstream_status"], "ambiguous")
        self.assertEqual(outside["upstream_status"], "unavailable")
        self.assertEqual(plan["checksum_snapshot"]["sha512"], hashlib.sha512(raw).hexdigest())
        self.assertEqual(plan["checksum_snapshot"]["malformed_lines"], 1)

    def test_checksum_same_name_multiple_paths_is_ambiguous(self):
        source = self.root / "SHA512SUMS"
        source.write_text("a" * 128 + "  test.sp3.gz\n")
        second = {**self.item, "path": self.item["path"].replace("2361", "2362"), "url": self.item["url"].replace("2361", "2362")}
        plan = {"files": [dict(self.item), second]}
        attach_checksums(plan, {"local_file": str(source), "source_url": ARCHIVE + "gnss/products/SHA512SUMS"})
        self.assertTrue(all(i["upstream_status"] == "ambiguous" for i in plan["files"]))

    def test_download_reuse_and_corrupt_file_recovery(self):
        http = FakeHTTP([Response(self.payload, headers={"ETag": '"v1"', "Content-Length": str(len(self.payload))})])
        record = download_one(self.root, self.item, http)
        self.assertEqual(record["status"], "complete")
        reused = download_one(self.root, self.item, FakeHTTP(), record)
        self.assertEqual(reused["action"], "reused")
        local_path(self.root, self.item["path"]).write_bytes(b"broken")
        repaired = download_one(self.root, self.item, FakeHTTP([Response(self.payload)]), record)
        self.assertEqual(repaired["status"], "complete")
        self.assertEqual(len(repaired["quarantined"]), 1)

    def test_interrupted_body_resumes_with_if_range(self):
        n = 20
        http = FakeHTTP(
            [
                Response(self.payload[:n], headers={"ETag": '"v1"'}, broken=True),
                Response(
                    self.payload[n:],
                    code=206,
                    headers={"ETag": '"v1"', "Content-Range": f"bytes {n}-{len(self.payload)-1}/{len(self.payload)}"},
                ),
            ]
        )
        result = download_one(self.root, self.item, http)
        self.assertEqual(result["status"], "complete")
        self.assertEqual(http.calls[1][1], {"Range": "bytes=20-", "If-Range": '"v1"'})

    def test_range_ignored_restarts_without_appending(self):
        http = FakeHTTP(
            [Response(self.payload[:20], headers={"ETag": '"v1"'}, broken=True), Response(self.payload, headers={"ETag": '"v2"'})]
        )
        result = download_one(self.root, self.item, http)
        self.assertEqual(result["status"], "complete")
        self.assertEqual(local_path(self.root, self.item["path"]).read_bytes(), self.payload)
        self.assertEqual(len(result["quarantined"]), 1)

    def test_wrong_range_and_changed_validator_are_rejected(self):
        for content_range, etag in (("bytes 0-19/20", '"v1"'), (f"bytes 20-{len(self.payload)-1}/{len(self.payload)}", '"v2"')):
            with tempfile.TemporaryDirectory() as root:
                http = FakeHTTP(
                    [
                        Response(self.payload[:20], headers={"ETag": '"v1"'}, broken=True),
                        Response(self.payload[20:], code=206, headers={"ETag": etag, "Content-Range": content_range}),
                    ]
                )
                self.assertEqual(download_one(root, self.item, http)["status"], "failed")

    def test_unvalidated_partial_is_quarantined_before_retry(self):
        http = FakeHTTP([Response(self.payload[:20], broken=True), Response(self.payload)])
        result = download_one(self.root, self.item, http)
        self.assertEqual(result["status"], "complete")
        self.assertEqual(http.calls[1][1], {})

    def test_upstream_mismatch_is_advisory_without_downgrade(self):
        for algorithm, width in (("sha512", 128), ("md5", 32)):
            with tempfile.TemporaryDirectory() as root:
                expected = {**self.item, "expected_digest": "0" * width, "digest_algorithm": algorithm}
                result = download_one(root, expected, FakeHTTP([Response(self.payload)]))
                self.assertEqual(result["status"], "complete")
                self.assertEqual(result["upstream_status"], "mismatch")
                self.assertEqual(result["expected_digest"], "0" * width)
                self.assertEqual(result["upstream_actual_digest"], hashlib.new(algorithm, self.payload).hexdigest())
                self.assertEqual(result["quarantined"], [])
                self.assertTrue(local_path(root, self.item["path"]).exists())
                reused = download_one(root, expected, FakeHTTP(), result)
                self.assertEqual(reused["action"], "reused")
                self.assertEqual(reused["upstream_status"], "mismatch")

    def test_advisory_mismatch_does_not_bypass_integrity_checks(self):
        expected = {**self.item, "expected_digest": "0" * 128, "digest_algorithm": "sha512"}
        path = self.root / "candidate.gz"
        for payload, message in (
            (self.payload[:-8] + b"\0" * 8, "Compressed product integrity"),
            (gzip.compress(b"not an SP3 header"), "Product header"),
        ):
            path.write_bytes(payload)
            with self.assertRaisesRegex(DownloadError, message):
                validate(path, {**expected, "size": len(payload)})
        path.write_bytes(self.payload)
        with self.assertRaisesRegex(DownloadError, "recorded local SHA-512"):
            validate(path, expected, "0" * 128)
        with self.assertRaisesRegex(DownloadError, "File size"):
            validate(path, {**expected, "size": len(self.payload) + 1})

    def test_html_is_auth_failure_even_with_http_200(self):
        http = FakeHTTP([Response(b"<html>login</html>", headers={"Content-Type": "text/html"})])
        with self.assertRaises(AuthError):
            download_one(self.root, self.item, http)
        self.assertTrue(http.stopped.is_set())

    def test_gzip_crc_failure_is_not_complete(self):
        corrupt = self.payload[:-8] + b"\x00" * 8
        result = download_one(self.root, self.item, FakeHTTP([Response(corrupt)]))
        self.assertEqual(result["status"], "failed")

    def test_404_does_not_retry(self):
        http = FakeHTTP([Response(code=404)])
        self.assertEqual(download_one(self.root, self.item, http)["status"], "failed")
        self.assertEqual(len(http.calls), 1)

    def test_lock_symlink_and_path_escape(self):
        link = self.root / "link"
        storage = self.root / "storage"
        storage.mkdir()
        link.symlink_to(storage, target_is_directory=True)
        with root_lock(link):
            with self.assertRaises(DownloadError):
                with root_lock(storage):
                    pass
        self.assertEqual(local_path(link, "a"), storage / "a")
        (storage / "escape").symlink_to(self.root, target_is_directory=True)
        for name in ("../a", "/a", "escape/a"):
            with self.assertRaises(DownloadError):
                local_path(link, name)

    def test_manifest_offline_verification_and_relocation(self):
        plan = {
            "sha512": "test",
            "start": "2025-04-06",
            "end": "2025-04-06",
            "files": [self.item],
            "slots": [],
            "latest_selected_dates": {},
        }
        with Progress(console=Console(file=io.StringIO()), disable=True) as progress:
            summary, code = fetch(plan, self.root, FakeHTTP([Response(self.payload)]), progress)
        self.assertEqual(code, 0)
        self.assertEqual(summary["files_complete"], 1)
        self.assertEqual(verify(self.root, lambda _: None), 0)
        local_path(self.root, self.item["path"]).write_bytes(b"broken")
        self.assertEqual(verify(self.root, lambda _: None), 1)
        records, _ = read_records(self.root)
        self.assertEqual(records[self.item["path"]]["status"], "corrupt")

    def test_mismatch_fetch_and_verify_succeed_with_warnings(self):
        expected = {**self.item, "expected_digest": "0" * 128, "digest_algorithm": "sha512"}
        plan = {
            "sha512": "test",
            "start": "2025-04-06",
            "end": "2025-04-06",
            "files": [expected],
            "slots": [],
            "latest_selected_dates": {},
        }
        terminal = io.StringIO()
        with Progress(console=Console(file=terminal, width=200), disable=True) as progress:
            summary, code = fetch(plan, self.root, FakeHTTP([Response(self.payload)]), progress)
        self.assertEqual(code, 0)
        self.assertEqual(summary["files_complete"], 1)
        self.assertEqual(summary["upstream_checksum"], {"mismatch": 1})
        self.assertIn("warning: upstream checksum mismatch", terminal.getvalue())
        messages = []
        self.assertEqual(verify(self.root, messages.append), 0)
        self.assertIn("warning: upstream checksum mismatch", messages[0])
        records, _ = read_records(self.root)
        self.assertEqual(records[self.item["path"]]["upstream_status"], "mismatch")

    def test_plan_tampering_is_rejected(self):
        plan = {"schema": 1, "files": [self.item]}
        plan["sha512"] = json_digest(plan)
        path = self.root / "plan.json"
        atomic_json(path, plan)
        self.assertEqual(load_plan(path)["files"], [self.item])
        plan["files"][0]["size"] += 1
        atomic_json(path, plan)
        with self.assertRaises(DownloadError):
            load_plan(path)

    def test_http_auth_is_scoped_and_redirects_rejected(self):
        netrc = self.root / "netrc"
        netrc.write_text("machine urs.earthdata.nasa.gov login example password secret\n")
        netrc.chmod(0o600)
        http = HTTP(netrc, DEFAULT_NETWORK, self.root / "cookies")
        http.throttle = lambda: None
        replies = iter(
            [
                Response(code=302, headers={"Location": "https://urs.earthdata.nasa.gov/oauth/authorize"}),
                Response(code=302, headers={"Location": self.item["url"]}),
                Response(self.payload),
            ]
        )
        calls = []

        def get(session, url, **kwargs):
            calls.append((url, kwargs.get("auth")))
            return next(replies)

        with patch.object(requests.Session, "get", get):
            http.request(self.item["url"]).close()
        self.assertEqual([auth for _, auth in calls], [None, ("example", "secret"), None])
        with patch.object(
            requests.Session, "get", return_value=Response(code=302, headers={"Location": "https://example.com/steal"})
        ):
            with self.assertRaises(DownloadError):
                http.request(self.item["url"])
        http.close()
        self.assertEqual((self.root / "cookies").stat().st_mode & 0o777, 0o600)

    def test_retry_after_and_429(self):
        self.assertEqual(retry_delay("3", 0), 3)
        self.assertEqual(retry_delay(None, 2), 4)
        netrc = self.root / "netrc"
        netrc.write_text("machine urs.earthdata.nasa.gov login example password secret\n")
        netrc.chmod(0o600)
        http = HTTP(netrc, DEFAULT_NETWORK, self.root / "cookies")
        http.throttle = lambda: None
        with (
            patch.object(http, "backoff") as backoff,
            patch.object(
                requests.Session, "get", side_effect=[Response(code=429, headers={"Retry-After": "3"}), Response(self.payload)]
            ),
        ):
            self.assertEqual(http.request(self.item["url"]).status_code, 200)
            backoff.assert_called_once_with("3", 0)

    def test_upstream_matches_sha512_and_explicit_md5(self):
        for algorithm in ("sha512", "md5"):
            expected = {
                **self.item,
                "expected_digest": hashlib.new(algorithm, self.payload).hexdigest(),
                "digest_algorithm": algorithm,
            }
            with tempfile.TemporaryDirectory() as root:
                result = download_one(root, expected, FakeHTTP([Response(self.payload)]))
                self.assertEqual(result["upstream_status"], "verified")

    def test_md5_evidence_uses_actual_digest_not_stale_expectation(self):
        expected = {**self.item, "expected_digest": "0" * 32, "digest_algorithm": "md5"}
        record = download_one(self.root, expected, FakeHTTP([Response(self.payload)]))
        self.assertEqual(upstream_evidence_status(expected, record), "mismatch")
        current = {**expected, "expected_digest": hashlib.md5(self.payload).hexdigest()}
        self.assertEqual(upstream_evidence_status(current, record), "verified")
        legacy = {**record, "upstream_status": "verified", "expected_digest": current["expected_digest"]}
        del legacy["upstream_actual_digest"]
        self.assertEqual(upstream_evidence_status(current, legacy), "verified")
        self.assertEqual(upstream_evidence_status(expected, legacy), "mismatch")
        self.assertEqual(upstream_evidence_status(current, {"sha512": record["sha512"]}), "available")

    def test_snapshot_extraction_of_matching_hash(self):
        source = self.root / "SHA512SUMS"
        expected = hashlib.sha512(self.payload).hexdigest()
        source.write_text(expected + " *test.sp3.gz\n")
        plan = {"files": [dict(self.item)]}
        attach_checksums(plan, {"local_file": str(source), "source_url": ARCHIVE + "gnss/products/SHA512SUMS"})
        self.assertEqual(plan["files"][0]["expected_digest"], expected)

    def test_required_missing_input_fails_fetch_but_optional_does_not(self):
        plan = {
            "sha512": "test",
            "start": "2025-04-06",
            "end": "2025-04-06",
            "files": [],
            "slots": [{"product": "station", "day": None, "week": None, "required": True, "issue": "external-input", "paths": []}],
            "latest_selected_dates": {},
        }
        with Progress(console=Console(file=io.StringIO()), disable=True) as progress:
            result, code = fetch(plan, self.root, FakeHTTP(), progress)
        self.assertEqual(code, 1)
        self.assertIsNone(result["required_inputs_available_through"])
        plan["slots"][0]["required"] = False
        with Progress(console=Console(file=io.StringIO()), disable=True) as progress:
            _, code = fetch(plan, self.root, FakeHTTP(), progress)
        self.assertEqual(code, 0)

    def test_static_change_prevents_reuse(self):
        record = download_one(self.root, self.item, FakeHTTP([Response(self.payload, headers={"ETag": '"old"'})]))
        changed = {**self.item, "etag": '"new"'}
        response = Response(self.payload, headers={"ETag": '"new"'})
        result = download_one(self.root, changed, FakeHTTP([response]), record)
        self.assertEqual(result["action"], "downloaded")

    def test_summary_new_checksum_keeps_complete_and_reports_mismatch(self):
        record = download_one(self.root, self.item, FakeHTTP([Response(self.payload)]))
        changed = {**self.item, "expected_digest": "0" * 128, "digest_algorithm": "sha512"}
        plan = {"start": "2025-04-06", "end": "2025-04-06", "files": [changed], "slots": [], "latest_selected_dates": {}}
        summary = summarize(plan, {record["path"]: record})
        self.assertEqual(summary["files_complete"], 1)
        self.assertEqual(summary["upstream_checksum"], {"mismatch": 1})
        changed["expected_digest"] = record["sha512"]
        self.assertEqual(summarize(plan, {record["path"]: record})["upstream_checksum"], {"verified": 1})

    def test_auth_error_sets_cli_exit_three(self):
        result = CliRunner().invoke(
            cli,
            [
                "plan",
                "--config",
                "config/products.example.toml",
                "--output",
                str(self.root / "plan.json"),
                "--netrc",
                str(self.root / "absent"),
            ],
        )
        self.assertEqual(result.exit_code, 3, result.output)

    def test_cddis_plaintext_callback_is_upgraded_before_request(self):
        netrc = self.root / "netrc"
        netrc.write_text("machine urs.earthdata.nasa.gov login example password secret\n")
        netrc.chmod(0o600)
        http = HTTP(netrc, DEFAULT_NETWORK, self.root / "cookies")
        http.throttle = lambda: None
        with patch.object(
            requests.Session,
            "get",
            side_effect=[
                Response(code=302, headers={"Location": self.item["url"].replace("https:", "http:")}),
                Response(self.payload),
            ],
        ) as get:
            http.request(self.item["url"]).close()
            self.assertTrue(all(call.args[0].startswith("https://") for call in get.call_args_list))

    def test_interrupt_sets_cli_exit_130(self):
        with (
            patch("neognss_observatory.cddis_download.HTTP"),
            patch("neognss_observatory.cddis_download.make_plan", side_effect=KeyboardInterrupt),
        ):
            result = CliRunner().invoke(
                cli, ["plan", "--config", "config/products.example.toml", "--output", str(self.root / "plan.json")]
            )
            self.assertEqual(result.exit_code, 130, result.output)

    def test_jsonlines_round_trip_and_record_layout(self):
        plan = {
            "schema": 2,
            "files": [self.item],
            "slots": [{"product": "orbit"}],
            "inventory": {"https://example.test/": {"time": "test"}},
        }
        plan["sha512"] = plan_digest(plan)
        path = self.root / "plan.jsonl"
        write_plan(path, plan)
        rows = [json.loads(line) for line in path.read_bytes().splitlines()]
        self.assertEqual([row["type"] for row in rows], ["header", "file", "slot", "inventory", "footer"])
        self.assertNotIn("files", rows[0]["data"])
        self.assertEqual(load_plan(path), plan)

    def test_jsonlines_rejects_tampering_truncation_and_trailing_rows(self):
        path = self.root / "plan.jsonl"
        write_plan(path, {"schema": 2, "files": [self.item], "slots": [], "inventory": {}})
        original = path.read_bytes()
        rows = original.splitlines(keepends=True)
        changed = json.loads(rows[1])
        changed["data"]["size"] += 1
        variants = [b"".join(rows[:-1]), rows[0] + json.dumps(changed).encode() + b"\n" + b"".join(rows[2:]), original + rows[1]]
        for value in variants:
            path.write_bytes(value)
            with self.assertRaises(DownloadError):
                load_plan(path)

    def test_jsonlines_empty_collections_and_legacy_conversion(self):
        path = self.root / "plan.jsonl"
        write_plan(path, {"schema": 1, "files": [], "slots": [], "inventory": {}})
        plan = load_plan(path)
        self.assertEqual(plan["schema"], 2)
        self.assertEqual(plan["files"], [])

    def test_jsonlines_write_failure_preserves_previous_file(self):
        path = self.root / "plan.jsonl"
        path.write_bytes(b"existing plan")
        with self.assertRaises(TypeError):
            write_plan(path, {"schema": 2, "files": [self.item, {"bad": object()}]})
        self.assertEqual(path.read_bytes(), b"existing plan")
        self.assertEqual(list(self.root.glob("plan.jsonl.*")), [])

    def test_inventory_progress_counts_cumulative_pending_and_reusable(self):
        first = "COD0MGXFIN_20250960000_01D_05M_ORB.SP3.gz"
        second = "COD0MGXFIN_20251030000_01D_05M_ORB.SP3.gz"
        first_item = item(self.payload, first)
        record = download_one(self.root, first_item, FakeHTTP([Response(self.payload)]))
        pages = {
            ARCHIVE + "gnss/products/2361/*?list": f"{first} {len(self.payload)}\n# Total number of files = 1\n",
            ARCHIVE + "gnss/products/2362/*?list": f"{second} {len(self.payload)}\n# Total number of files = 1\n",
        }
        config = {"start": "2025-04-06", "end": "2025-04-13", "guard_days": 0, "products": [product()], "network": DEFAULT_NETWORK}
        updates = []
        make_plan(
            config,
            "example",
            FakeHTTP(pages=pages),
            records={first_item["path"]: record},
            root=self.root,
            inventory_progress=updates.append,
        )
        self.assertEqual(
            [(s["completed"], s["selected"], s["pending"], s["reusable"]) for s in updates],
            [(0, 0, 0, 0), (1, 1, 0, 1), (2, 2, 1, 1), (2, 2, 1, 1)],
        )
        local_path(self.root, first_item["path"]).unlink()
        updates.clear()
        make_plan(
            config,
            "example",
            FakeHTTP(pages=pages),
            records={first_item["path"]: record},
            root=self.root,
            inventory_progress=updates.append,
        )
        self.assertEqual(updates[-1]["pending"], 2)

    def test_inventory_checksum_conflict_does_not_increase_pending_count(self):
        name = "COD0MGXFIN_20250960000_01D_05M_ORB.SP3.gz"
        candidate = item(self.payload, name)
        record = download_one(self.root, candidate, FakeHTTP([Response(self.payload)]))
        snapshot = self.root / "SHA512SUMS"
        snapshot.write_text("0" * 128 + "  " + name + "\n")
        pages = {ARCHIVE + "gnss/products/2361/*?list": f"{name} {len(self.payload)}\n# Total number of files = 1\n"}
        config = {
            "start": "2025-04-06",
            "end": "2025-04-06",
            "guard_days": 0,
            "products": [product()],
            "network": DEFAULT_NETWORK,
            "checksum": {"source_url": ARCHIVE + "gnss/products/SHA512SUMS", "local_file": str(snapshot)},
        }
        updates = []
        make_plan(
            config,
            "test",
            FakeHTTP(pages=pages),
            records={candidate["path"]: record},
            root=self.root,
            inventory_progress=updates.append,
        )
        self.assertEqual(updates[-2]["pending"], 0)
        self.assertEqual(updates[-1]["pending"], 0)

    def test_terminal_inventory_progress_and_jsonlines_cli_output(self):
        name = "COD0MGXFIN_20250960000_01D_05M_ORB.SP3.gz"
        pages = {ARCHIVE + "gnss/products/2361/*?list": f"{name} {len(self.payload)}\n# Total number of files = 1\n"}
        config = {"start": "2025-04-06", "end": "2025-04-06", "guard_days": 0, "products": [product()], "network": DEFAULT_NETWORK}
        output = self.root / "plan.jsonl"
        terminal = io.StringIO()
        http = FakeHTTP(pages=pages)
        http.close = lambda: None
        with (
            patch("neognss_observatory.cddis_download.HTTP", return_value=http),
            patch("neognss_observatory.cddis_download.read_config", return_value=(config, "test")),
            patch("neognss_observatory.cddis_download.console", Console(file=terminal, force_terminal=True, width=160)),
        ):
            result = CliRunner().invoke(
                cli, ["plan", "--config", "config/products.example.toml", "--output", str(output), "--root", str(self.root)]
            )
        self.assertEqual(result.exit_code, 0, result.output)
        self.assertIn("1 need download", terminal.getvalue())
        self.assertIn("1/1 weeks", terminal.getvalue())
        self.assertEqual(json.loads(result.stdout)["files_complete"], 0)
        self.assertEqual(len(load_plan(output)["files"]), 1)

    def test_directory_body_retries_without_combining_partial_responses(self):
        netrc = self.root / "netrc"
        netrc.write_text("machine urs.earthdata.nasa.gov login example password secret\n")
        netrc.chmod(0o600)
        http = HTTP(netrc, {**DEFAULT_NETWORK, "retries": 1}, self.root / "cookies")
        with (
            patch.object(http, "request", side_effect=[Response(b"partial", broken=True), Response(b"complete")]),
            patch.object(http, "backoff") as backoff,
        ):
            self.assertEqual(http.text(ARCHIVE + "gnss/products/2361/*?list"), "complete")
            backoff.assert_called_once_with(None, 0)

    def test_directory_invalid_utf8_retries_and_reports_safe_path(self):
        netrc = self.root / "netrc"
        netrc.write_text("machine urs.earthdata.nasa.gov login example password secret\n")
        netrc.chmod(0o600)
        http = HTTP(netrc, {**DEFAULT_NETWORK, "retries": 1}, self.root / "cookies")
        with patch.object(http, "request", side_effect=[Response(b"\xff"), Response(b"\xff")]), patch.object(http, "backoff"):
            with self.assertRaises(DownloadError) as error:
                http.text(ARCHIVE + "gnss/products/2361/*?private-query")
        self.assertIn("gnss/products/2361/", str(error.exception))
        self.assertIn("UnicodeDecodeError", str(error.exception))
        self.assertNotIn("private-query", str(error.exception))

    def test_inventory_resumes_after_failure_without_refetching_completed_week(self):
        first = "COD0MGXFIN_20250960000_01D_05M_ORB.SP3.gz"
        second = "COD0MGXFIN_20251030000_01D_05M_ORB.SP3.gz"
        first_url = ARCHIVE + "gnss/products/2361/*?list"
        second_url = ARCHIVE + "gnss/products/2362/*?list"
        pages = {
            first_url: f"{first} 100\n# Total number of files = 1\n",
            second_url: f"{second} 100\n# Total number of files = 1\n",
        }
        config = {"start": "2025-04-06", "end": "2025-04-13", "guard_days": 0, "products": [product()], "network": DEFAULT_NETWORK}
        checkpoint = self.root / "inventory.sqlite"
        http = FakeHTTP(pages=pages)
        original = http.text

        def fail_second(url):
            if url == second_url:
                raise DownloadError("Simulated inventory interruption")
            return original(url)

        http.text = fail_second
        with self.assertRaises(DownloadError), InventoryCache(checkpoint, "same-config") as cache:
            make_plan(config, "test", http, checkpoint=cache)
        resumed = FakeHTTP(pages=pages)
        with InventoryCache(checkpoint, "same-config") as cache:
            plan = make_plan(config, "test", resumed, checkpoint=cache)
            self.assertEqual(cache.hits, 1)
            self.assertEqual(len(plan["files"]), 2)
        self.assertNotIn((first_url, {}), resumed.calls)
        self.assertIn((second_url, {}), resumed.calls)

    def test_cache_invalidates_completed_changed_or_explicitly_refreshed_runs(self):
        path = self.root / "inventory.sqlite"
        for mode in ("complete", "changed", "refresh"):
            with InventoryCache(path, "original", refresh=True) as cache:
                cache.put("url", "listing", {}, {"time": "old"})
                if mode == "complete":
                    cache.complete()
            with InventoryCache(path, "new" if mode == "changed" else "original", refresh=mode == "refresh") as cache:
                self.assertIsNone(cache.get("url", "listing"))

    def test_invalid_listing_is_not_checkpointed(self):
        url = ARCHIVE + "gnss/products/2361/*?list"
        with InventoryCache(self.root / "inventory.sqlite", "test") as cache:
            inventory = Inventory(FakeHTTP(pages={url: "a.gz 100\n# Total number of files = 2\n"}), cache)
            with self.assertRaises(DownloadError):
                inventory.listing("gnss/products/2361/")
            self.assertIsNone(cache.get(url, "listing"))

    def test_numeric_directory_index_is_checkpointed_and_planner_lock_is_exclusive(self):
        path = self.root / "inventory.sqlite"
        url = ARCHIVE + "gnss/products/"
        with InventoryCache(path, "test") as cache:
            listing = Inventory(FakeHTTP(pages={url: '<a title="DataDirectory" href="2361">2361</a>'}), cache)
            self.assertEqual(listing.directories("gnss/products/"), [2361])
            with self.assertRaises(DownloadError), InventoryCache(path, "test"):
                pass
        with InventoryCache(path, "test") as cache:
            offline = FakeHTTP()
            self.assertEqual(Inventory(offline, cache).directories("gnss/products/"), [2361])
            self.assertEqual(offline.calls, [])


if __name__ == "__main__":
    unittest.main()
