"""Exclusive publication, provenance, and read-back verification."""

import fcntl
import hashlib
import json
import os
import shutil
import uuid
from pathlib import Path

from tqdm import tqdm

from .ubx_restitch import sha256, source_identity, write_json, write_plan


def publish(plan, output):
    output.mkdir(parents=True, exist_ok=True)
    lock = output / ".restitch.lock"
    with lock.open("a") as lockfile:
        fcntl.flock(lockfile, fcntl.LOCK_EX | fcntl.LOCK_NB)
        serialized = json.dumps(plan, sort_keys=True, separators=(",", ":")).encode()
        plan_hash = hashlib.sha256(serialized).hexdigest()
        run = output / "run.json"
        if run.exists():
            if json.loads(run.read_text())["plan_sha256"] != plan_hash:
                raise ValueError("Output directory belongs to a different reconstruction plan")
        else:
            if any(p.name != lock.name for p in output.iterdir()):
                raise ValueError("Output directory is not empty and has no matching run manifest")
            write_json(run, {"schema": plan["schema"], "time_scale": "GPST", "plan_sha256": plan_hash, "status": "started"})
            write_plan(output / "plan.jsonl", plan)
            indexes = output / "provenance/indexes"
            indexes.mkdir(parents=True)
            for source in plan["sources"]:
                destination = indexes / (source["sha256"] + ".idx")
                shutil.copyfile(source["index"], destination)
                if sha256(destination) != source["index_sha256"]:
                    raise ValueError(f"Index copy failed verification: {destination}")
            write_json(output / "provenance/ready.json", {"plan_sha256": plan_hash})
        if not (output / "provenance/ready.json").exists():
            raise ValueError("Provenance initialization was interrupted; inspect this output directory before recovery")
        state = output / ".artifacts"
        state.mkdir(exist_ok=True)
        for source in plan["sources"]:
            if source_identity(Path(source["path"])) != {k: source[k] for k in ("path", "size", "mtime_ns")}:
                raise ValueError(f"Source changed: {source['path']}")
        completed = []
        with tqdm(total=sum(a["size"] for a in plan["artifacts"]), unit="B", unit_scale=True, desc="Write + verify") as progress:
            for artifact in plan["artifacts"]:
                name = artifact["name"]
                final = output / name
                metadata = state / (hashlib.sha256(name.encode()).hexdigest() + ".json")
                if metadata.exists():
                    record = json.loads(metadata.read_text())
                    if not final.is_file() or final.stat().st_size != artifact["size"] or sha256(final) != record["sha256"]:
                        raise ValueError(f"Existing output failed verification: {final}")
                    completed.append(record)
                    progress.update(artifact["size"])
                    continue
                if final.exists() or final.is_symlink():
                    raise ValueError(f"Refusing to overwrite output without completion metadata: {final}")
                final.parent.mkdir(parents=True, exist_ok=True)
                temporary = final.with_name(final.name + f".{uuid.uuid4().hex}.partial")
                digest = hashlib.sha256()
                try:
                    with temporary.open("xb") as target:
                        for span in artifact["spans"]:
                            with Path(span["source"]).open("rb") as source:
                                source.seek(span["begin"])
                                remaining = span["end"] - span["begin"]
                                while remaining:
                                    data = source.read(min(8 * 1024 * 1024, remaining))
                                    if not data:
                                        raise ValueError(f"Source truncated: {span['source']}")
                                    target.write(data)
                                    digest.update(data)
                                    remaining -= len(data)
                                    progress.update(len(data))
                        target.flush()
                        os.fsync(target.fileno())
                    if sha256(temporary) != digest.hexdigest():
                        raise ValueError(f"Read-back checksum mismatch: {temporary}")
                    # Hard-link publication fails atomically if the destination exists.
                    os.link(temporary, final)
                    temporary.unlink()
                    record = {**artifact, "sha256": digest.hexdigest(), "verified": True}
                    write_json(metadata, record)
                    completed.append(record)
                except BaseException:
                    # Keep partial artifacts for inspection; never present them as complete.
                    raise
        for source in plan["sources"]:
            if source_identity(Path(source["path"])) != {k: source[k] for k in ("path", "size", "mtime_ns")}:
                raise ValueError(f"Source changed during output: {source['path']}")
        completion = output / "completed.json"
        result = {
            "plan_sha256": plan_hash,
            "status": "complete",
            "artifacts": len(completed),
            "bytes": sum(a["size"] for a in completed),
            "gpst_segments": sum(a["kind"] == "gpst_segment" for a in completed),
            "unassigned_files": sum(a["kind"] == "unassigned_frames" for a in completed),
            "joins": len(plan["joins"]),
        }
        if not completion.exists():
            write_json(completion, result)
        return result
