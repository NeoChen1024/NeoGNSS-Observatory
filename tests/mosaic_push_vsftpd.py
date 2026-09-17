#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Opt-in real-vsftpd integration scenarios in a private Linux user/network namespace.

Run with the repository venv; see docs/mosaic-push.md for invocation.
"""
import copy
import hashlib
import json
import logging
import lzma
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from datetime import date, timedelta
from pathlib import Path
from unittest.mock import patch

import click

import neognss_observatory.mosaic_push as mod
from neognss_observatory.mosaic_push import (
    Mirror,
    PushError,
    expand_path,
    expected_name,
    read_config,
    utc_now,
)


@click.command()
@click.option("--vsftpd", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--suite", type=click.Choice(["storage", "resume"]), default="storage")
def cli(vsftpd, suite):
    """Exercise storage pressure or interrupted FTP/FTPS publication."""
    mapping = Path("/proc/self/uid_map").read_text().split()
    if os.geteuid() != 0 or (len(mapping) != 3 or mapping[0] != "0" or mapping[2] != "1"):
        raise click.ClickException("Run in an unshare --user --map-root-user --net namespace")
    if {name for _, name in socket.if_nameindex()} != {"lo"}:
        raise click.ClickException("Use a private network namespace with only loopback")
    root = Path(tempfile.mkdtemp(prefix="ngo-mosaic-vsftpd-"))
    cert, key = root / "cert.pem", root / "key.pem"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-days",
            "1",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:127.0.0.1",
        ],
        check=True,
        capture_output=True,
    )
    (root / "empty").mkdir()
    servers = []

    def start_server(name, port, tls=False, implicit=False):
        directory = root / name
        directory.mkdir(exist_ok=True)
        log = root / (name + ".log")
        lines = {
            "listen": "YES",
            "listen_ipv6": "NO",
            "listen_address": "127.0.0.1",
            "listen_port": port,
            "background": "NO",
            "anonymous_enable": "YES",
            "local_enable": "NO",
            "no_anon_password": "YES",
            "write_enable": "YES",
            "anon_upload_enable": "YES",
            "anon_mkdir_write_enable": "YES",
            "anon_other_write_enable": "YES",
            "anon_world_readable_only": "NO",
            "run_as_launching_user": "YES",
            "ftp_username": "root",
            "nopriv_user": "root",
            "seccomp_sandbox": "NO",
            "isolate": "NO",
            "isolate_network": "NO",
            "anon_root": directory,
            "secure_chroot_dir": root / "empty",
            "xferlog_enable": "YES",
            "log_ftp_protocol": "YES",
            "vsftpd_log_file": log,
            "ssl_enable": "YES" if tls else "NO",
            "allow_anon_ssl": "YES",
            "force_anon_data_ssl": "YES" if tls else "NO",
            "force_anon_logins_ssl": "YES" if tls else "NO",
            "require_ssl_reuse": "YES",
            "rsa_cert_file": cert,
            "rsa_private_key_file": key,
            "implicit_ssl": "YES" if implicit else "NO",
        }
        conf = root / (name + ".conf")
        conf.write_text("".join(f"{k}={v}\n" for k, v in lines.items()))
        output = (root / (name + ".process.log")).open("wb")
        proc = subprocess.Popen([str(vsftpd), str(conf)], stdout=output, stderr=subprocess.STDOUT)
        servers.append((proc, output))
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError((root / (name + ".process.log")).read_text())
            try:
                with socket.create_connection(("127.0.0.1", port), 0.1):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise RuntimeError("server failed to listen")
        return directory, proc

    def commands(name, command):
        p = root / (name + ".log")
        return p.read_text().count(f'"{command} ') if p.exists() else 0

    logs = []

    class Capture(logging.Handler):
        def emit(self, r):
            logs.append(r.getMessage())

    logging.getLogger("neognss_observatory").setLevel(logging.INFO)
    logging.getLogger("neognss_observatory").addHandler(Capture())

    MiB = 1024**2
    GiB = 1024**3

    def run_cycle(m, expected_failure=False):
        peaks = [0]
        errors = []
        t = threading.Thread(target=m.worker)
        t.start()
        deadline = time.monotonic() + 180
        while t.is_alive():
            m.poll()
            peaks[0] = max(peaks[0], m.usage())
            t.join(0.02)
            if time.monotonic() > deadline:
                m.stop.set()
                m.poll()
                t.join(10)
                raise AssertionError("deadlock")
        assert m.failed == expected_failure, (m.failed, expected_failure, logs[-8:])
        assert m.limit is None or peaks[0] <= m.limit, (peaks, m.limit)
        return peaks[0]

    def load_case(name, source, destination, filler=0):
        c = json.loads(Path("config/mosaic-push.example.json").read_text())
        c["archive_root"] = str(root / name)
        Path(c["archive_root"]).mkdir()
        if filler:
            with (Path(c["archive_root"]) / "unmanaged-space.bin").open("wb") as f:
                f.truncate(filler)
        c["source"].update(host="127.0.0.1", port=21211, path=str(source))
        c["destinations"] = {
            "primary": dict(
                enabled=True,
                host="127.0.0.1",
                port=21212,
                username="anonymous",
                password="",
                tls="explicit",
                verify_tls=True,
                ca_file=str(cert),
                path=str(destination),
            )
        }
        c["storage"] = {"limit_gib": 8}
        c["compression"].update(preset=0, threads=1)
        c["network"].update(connect_timeout_seconds=2, stall_timeout_seconds=3, completion_timeout_seconds=3)
        c["output"] = {"mode": "0644", "dirmode": "0755"}
        cfg = root / (name + ".json")
        cfg.write_text(json.dumps(c))
        return Mirror(read_config(cfg), threading.Event()), c, cfg

    def make_source(parent, name, payload, count=7):
        source = parent / name
        source.mkdir()
        days = [utc_now().date() - timedelta(days=i) for i in range(count + 3, 1, -1)]
        for i, day in enumerate(days):
            d = source / expand_path("%y%j", day)
            d.mkdir()
            path = d / expected_name("bee_", day)
            if i < 2:
                path.write_bytes(b"old excluded")
            else:
                os.link(payload, path)
        return source, days[2:]

    def verify(m, dest, expected):
        for rel, r in m.state["files"].items():
            side = Path(rel).with_suffix(".sha512")
            want = f"{expected}  {Path(rel).name.removesuffix('.xz')}\n".encode()
            assert (dest / side.name).read_bytes() == want
            assert hashlib.sha512(lzma.decompress((dest / Path(rel).name).read_bytes())).hexdigest() == expected
            local = m.root / rel
            if not r.get("evicted"):
                assert local.with_suffix(".sha512").read_bytes() == want
                assert local.stat().st_mode & 0o777 == 0o644
            else:
                assert not local.exists() and not local.with_suffix(".sha512").exists()

    try:
        if suite == "storage":
            receiver, _ = start_server("receiver", 21211)
            remote, _ = start_server("remote", 21212, True)
            random_file = root / "random.sbf"
            zero_file = root / "zero.sbf"
            with random_file.open("wb") as f:
                for _ in range(64):
                    f.write(os.urandom(MiB))
            with zero_file.open("wb") as f:
                f.truncate(64 * MiB)
            for label, payload in [("compressible", zero_file), ("incompressible", random_file)]:
                source, days = make_source(receiver, label, payload)
                dest = remote / label
                dest.mkdir()
                m, c, cfg = load_case(label, source, dest, int(7.25 * GiB))
                peak = run_cycle(m)
                expected = hashlib.sha512(payload.read_bytes()).hexdigest()
                verify(m, dest, expected)
                evicted = sum(bool(r.get("evicted")) for r in m.state["files"].values())
                assert bool(evicted) == (label == "incompressible"), evicted
                assert (m.root / "unmanaged-space.bin").stat().st_size == int(7.25 * GiB)
                before = commands("receiver", "RETR")
                run_cycle(m)
                assert commands("receiver", "RETR") == before
                print("PASS", label, "8 GiB cap, 7.25 GiB unmanaged occupancy; peak", peak, "evicted", evicted, flush=True)
                if label == "incompressible":
                    saved = (m, c, cfg, source, days, dest)
            m, c, cfg, source, days, dest = saved
            # Receiver presence includes excluded dates; active data never downloaded.
            rel = next(rel for rel, r in m.state["files"].items() if r.get("evicted"))
            day = date.fromisoformat(m.state["files"][rel]["day"])
            folder = source / expand_path("%y%j", day)
            shutil.rmtree(folder)
            run_cycle(m)
            assert rel in m.state["files"] and m.state["files"][rel]["absent_scans"] == 1
            with patch.object(m, "inventory", side_effect=OSError("injected failed inventory")):
                run_cycle(m, True)
            assert rel in m.state["files"]
            run_cycle(m)
            assert rel not in m.state["files"]
            print("PASS tombstone GC needs two complete absence scans; failed inventory cannot collect state", flush=True)
            # Local archive set larger than receiver retention; existing archives still forward.
            source2, days2 = make_source(receiver, "large-local", random_file, count=2)
            dest2 = remote / "large-local"
            dest2.mkdir()
            large, lc, lcfg = load_case("large-local", source2, dest2)
            run_cycle(large)
            assert not any(r.get("evicted") for r in large.state["files"].values())
            for d in days2:
                shutil.rmtree(source2 / expand_path("%y%j", d))
            run_cycle(large)
            assert len(large.state["files"]) == 2
            print("PASS local retention outlives receiver files without state GC or redownload", flush=True)
            # Pending uploads occupy capacity until remote recovers.
            source3, days3 = make_source(receiver, "offline", random_file)
            dest3 = remote / "offline"
            dest3.mkdir()
            blocked, bc, bcfg = load_case("offline", source3, dest3, int(7.25 * GiB))
            bc["destinations"]["primary"]["port"] = 21214
            bcfg.write_text(json.dumps(bc))
            blocked = Mirror(read_config(bcfg), threading.Event())
            run_cycle(blocked, True)
            owned = [p for p in blocked.root.rglob("*.xz")]
            assert owned and len(owned) < len(days3)
            assert not any(r.get("evicted") for r in blocked.state["files"].values())
            ids = {p: mod.file_identity(p) for p in owned}
            run_cycle(blocked, True)
            assert all(mod.file_identity(p) == i for p, i in ids.items())
            bc["destinations"]["primary"]["port"] = 21212
            bcfg.write_text(json.dumps(bc))
            blocked = Mirror(read_config(bcfg), threading.Event())
            run_cycle(blocked)
            assert all(blocked.delivered(r) for r in blocked.state["files"].values())
            print("PASS stalled target protects pending archives, bounded retries return, recovery drains backlog", flush=True)
            # Insufficient real free space never removes unknown files or starts RETR.
            source4, _ = make_source(receiver, "lowfree", zero_file, count=1)
            dest4 = remote / "lowfree"
            dest4.mkdir()
            low, _, _ = load_case("lowfree", source4, dest4)
            before = commands("receiver", "RETR")
            with patch.object(mod.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(1000, 999, 1)):
                run_cycle(low, True)
            assert commands("receiver", "RETR") == before
            print("PASS physical disk free-space guard independently blocks admission", flush=True)
            for value in [0, 7.99, -1, True, "8", float("inf")]:
                bc["storage"]["limit_gib"] = value
                bcfg.write_text(json.dumps(bc))
                try:
                    read_config(bcfg)
                except PushError:
                    pass
                else:
                    raise AssertionError(value)
            print("PASS public storage limit rejects values below 8 GiB and invalid types", flush=True)
            # Source larger than a complete workspace: reject before touching retained archives.
            giantday = utc_now().date() - timedelta(days=1)
            giantdir = source2 / expand_path("%y%j", giantday)
            giantdir.mkdir()
            with (giantdir / expected_name("bee_", giantday)).open("wb") as f:
                f.truncate(5 * GiB)
            before = commands("receiver", "RETR")
            retained = {p: mod.file_identity(p) for p in large.root.rglob("*.xz")}
            run_cycle(large, True)
            assert commands("receiver", "RETR") == before
            assert all(mod.file_identity(p) == v for p, v in retained.items())
            print("PASS oversized source rejected before RETR and before eviction", flush=True)
            # A lowered quota cannot justify deleting unknown files.
            with (low.root / "unmanaged-too-large").open("wb") as f:
                f.truncate(9 * GiB)
            low.worker()
            assert low.failed and (low.root / "unmanaged-too-large").stat().st_size == 9 * GiB
            print("PASS already-over-limit unmanaged files preserved with failure", flush=True)
            # Local-only eviction and an interrupted deletion recover without re-downloading.
            localrel = next(rel for rel, r in m.state["files"].items() if not r.get("evicted"))
            record = copy.deepcopy(m.state["files"][localrel])
            record["evicting"] = True
            m.save_record(localrel, record)
            m = Mirror(read_config(cfg), threading.Event())
            assert m.state["files"][localrel]["evicted"]
            assert not (m.root / localrel).exists()
            print("PASS durable eviction intent recovers after interruption", flush=True)
        else:
            receiver, _ = start_server("receiver", 21211)
            remote, _ = start_server("remote", 21212, True)
            payload = root / "payload"
            payload.write_bytes(os.urandom(4 * MiB))
            source, days = make_source(receiver, "resume", payload, count=1)
            dest = remote / "resume"
            dest.mkdir()
            m, c, cfg = load_case("resume", source, dest)
            c["schedule"]["retry_seconds"] = 1
            cfg.write_text(json.dumps(c))
            m = Mirror(read_config(cfg), threading.Event())
            real_advance = mod.TransferWatch.advance
            broken = [False]

            def fail_download(w, n):
                real_advance(w, n)
                if w.label.startswith("Download") and w.bytes >= MiB and not broken[0]:
                    broken[0] = True
                    raise OSError("injected disconnect after download progress")

            with patch.object(mod.TransferWatch, "advance", fail_download):
                run_cycle(m, True)
            rel = m.relative(days[0])
            part = m.raw_path(rel).with_name(m.raw_path(rel).name + ".part")
            assert 0 < part.stat().st_size < payload.stat().st_size
            before = commands("receiver", "RETR")
            run_cycle(m, True)
            assert commands("receiver", "RETR") == before
            real_time = time.time
            # Advance only wall-clock retry deadlines; monotonic timeouts remain real.
            with patch.object(mod.time, "time", side_effect=lambda: real_time() + 100):
                run_cycle(m)
            assert '"REST ' in (root / "receiver.log").read_text()
            want = hashlib.sha512(payload.read_bytes()).hexdigest()
            verify(m, dest, want)
            print("PASS RETR resume uses REST, persisted partial, SHA512 full-file verification and backoff", flush=True)
            source, days = make_source(receiver, "upload-resume", payload, count=1)
            dest = remote / "upload-resume"
            dest.mkdir()
            m, c, cfg = load_case("upload-resume", source, dest)
            broken[0] = False

            def fail_upload(w, n):
                real_advance(w, n)
                if w.label.startswith("Upload") and ".xz." in w.label and w.bytes >= MiB and not broken[0]:
                    broken[0] = True
                    raise OSError("injected disconnect after upload progress")

            with patch.object(mod.TransferWatch, "advance", fail_upload):
                run_cycle(m, True)
            remote_part = next(dest.glob("*.xz.ngo-mosaic-push.part"))
            assert 0 < remote_part.stat().st_size < payload.stat().st_size
            m = Mirror(read_config(cfg), threading.Event())
            with patch.object(mod.time, "time", side_effect=lambda: real_time() + 10000):
                run_cycle(m)
            assert '"REST ' in (root / "remote.log").read_text()
            verify(m, dest, want)
            print("PASS FTPS upload resumes after daemon restart and publishes matching decompressed SHA512", flush=True)
            # Complete xz upload succeeds, checksum publication fails; retry must only send checksum.
            source, days = make_source(receiver, "sidecar-retry", payload, count=1)
            dest = remote / "sidecar-retry"
            dest.mkdir()
            m, c, cfg = load_case("sidecar-retry", source, dest)
            real_rename = mod.ftplib.FTP.rename

            def fail_sidecar(ftp, a, b):
                if b.endswith(".sha512"):
                    raise OSError("injected checksum rename failure")
                return real_rename(ftp, a, b)

            with patch.object(mod.ftplib.FTP, "rename", fail_sidecar):
                run_cycle(m, True)
            rel = m.relative(days[0])
            assert not m.delivered(m.state["files"][rel])
            old = next(dest.glob("*.xz")).stat()
            with patch.object(mod.time, "time", side_effect=lambda: real_time() + 10000):
                run_cycle(m)
            assert next(dest.glob("*.xz")).stat().st_mtime_ns == old.st_mtime_ns
            verify(m, dest, want)
            print("PASS checksum failure protects archive, retry avoids reuploading completed xz", flush=True)
            # Lost FTP completion reply leaves a full owned .part, which can be published on retry.
            source, days = make_source(receiver, "lost-reply", payload, count=1)
            dest = remote / "lost-reply"
            dest.mkdir()
            m, c, cfg = load_case("lost-reply", source, dest)
            real_reply = mod.completion_reply

            def lost_reply(ftp, name, network):
                real_reply(ftp, name, network)
                if ".xz.ngo-mosaic-push.part" in name:
                    raise OSError("injected lost completion reply")

            with patch.object(mod, "completion_reply", lost_reply):
                run_cycle(m, True)
            before = (root / "remote.log").read_text().count('"STOR ')
            with patch.object(mod.time, "time", side_effect=lambda: real_time() + 10000):
                run_cycle(m)
            assert (root / "remote.log").read_text().count('"STOR ') == before + 1  # checksum only
            verify(m, dest, want)
            print("PASS full partial after lost reply publishes without retransmitting xz", flush=True)
            # A source that changes after an interrupted download cannot reuse its old prefix.
            source, days = make_source(receiver, "changed-partial", payload, count=1)
            dest = remote / "changed-partial"
            dest.mkdir()
            m, c, cfg = load_case("changed-partial", source, dest)
            broken[0] = False
            with patch.object(mod.TransferWatch, "advance", fail_download):
                run_cycle(m, True)
            path = source / expand_path("%y%j", days[0]) / expected_name("bee_", days[0])
            path.unlink()
            path.write_bytes(os.urandom(4 * MiB))
            os.utime(path, (real_time() + 20, real_time() + 20))
            before = (root / "receiver.log").read_text().count('"REST ')
            run_cycle(m)
            assert (root / "receiver.log").read_text().count('"REST ') == before
            verify(m, dest, hashlib.sha512(path.read_bytes()).hexdigest())
            print("PASS changed source invalidates retained download prefix and retry backoff", flush=True)
            # A failed state write must not strand the capacity-limited coordinator.
            source, days = make_source(receiver, "state-full", payload, count=1)
            dest = remote / "state-full"
            dest.mkdir()
            m, c, cfg = load_case("state-full", source, dest)
            real_save = m.save_record

            def fail_worker_save(*args):
                if threading.current_thread().name == "mosaic-download":
                    raise OSError(28, "injected ENOSPC while persisting download state")
                return real_save(*args)

            with patch.object(m, "save_record", fail_worker_save):
                run_cycle(m, True)
            assert not m.pinned and not m.reservations
            run_cycle(m)
            verify(m, dest, want)
            print("PASS ENOSPC saving retry state terminates the cycle and recovers without deadlock", flush=True)
            # Compression output cannot write past its granted reservation.
            source, days = make_source(receiver, "xz-bound", payload, count=1)
            dest = remote / "xz-bound"
            dest.mkdir()
            m, c, cfg = load_case("xz-bound", source, dest)
            real_compress = m.compress

            def bounded_compress(relative, record):
                with patch.object(m, "xz_budget", return_value=1024):
                    return real_compress(relative, record)

            with patch.object(m, "compress", bounded_compress):
                run_cycle(m, True)
            rel = m.relative(days[0])
            assert m.raw_path(rel).exists() and not (m.root / rel).exists()
            assert not (m.root / rel).with_name(Path(rel).name + ".part").exists()
            before = commands("receiver", "RETR")
            run_cycle(m)
            assert commands("receiver", "RETR") == before
            verify(m, dest, want)
            print("PASS xz output bound abort preserves raw; retry does not redownload", flush=True)
            # The same local-only workflow supports larger and unlimited retention.
            for index, limit in enumerate((8, 16, None)):
                source, days = make_source(receiver, f"local-only-{index}", payload, count=1)
                dest = remote / f"local-only-{index}"
                dest.mkdir()
                m, c, cfg = load_case(f"local-only-{index}", source, dest)
                c["destinations"] = {}
                c["storage"]["limit_gib"] = limit
                cfg.write_text(json.dumps(c))
                m = Mirror(read_config(cfg), threading.Event())
                run_cycle(m)
                rel = m.relative(days[0])
                assert (m.root / rel).with_suffix(".sha512").read_text().startswith(want)
                assert not list(dest.iterdir())
                assert m.delivered(m.state["files"][rel])
                m.state["files"][rel]["evicting"] = True
                m.save()
                m = Mirror(read_config(cfg), threading.Event())
                before = commands("receiver", "RETR")
                run_cycle(m)
                assert commands("receiver", "RETR") == before
            print("PASS local-only 8 GiB, 16 GiB and unlimited retention, checksum and eviction recovery", flush=True)
            # Return to a remotely published fixture for the same-size revision check.
            source, days = make_source(receiver, "same-size", payload, count=1)
            dest = remote / "same-size"
            dest.mkdir()
            m, c, cfg = load_case("same-size", source, dest)
            run_cycle(m)
            # Same-length raw rewrite is detected by mtime; recompressed stream must replace old remote bytes.
            oldxz = next(dest.glob("*.xz")).read_bytes()
            path = source / expand_path("%y%j", days[0]) / expected_name("bee_", days[0])
            path.unlink()
            path.write_bytes(os.urandom(4 * MiB))
            os.utime(path, (real_time() + 20, real_time() + 20))
            run_cycle(m)
            newxz = next(dest.glob("*.xz")).read_bytes()
            assert oldxz != newxz and len(oldxz) == len(newxz)
            verify(m, dest, hashlib.sha512(path.read_bytes()).hexdigest())
            print("PASS same-size source revision replaces same-size remote archive and checksum", flush=True)
    finally:
        for proc, out in servers:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            out.close()
        shutil.rmtree(root)


if __name__ == "__main__":
    cli()
