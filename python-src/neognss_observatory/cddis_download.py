#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Plan, download and verify external GNSS products."""

import contextlib
import json
import sqlite3
from pathlib import Path

import click
from rich.console import Console
from rich.filesize import decimal
from rich.progress import (
    BarColumn,
    DownloadColumn,
    Progress,
    TextColumn,
    TimeRemainingColumn,
    TransferSpeedColumn,
)
from rich.text import Text

from neognss_observatory.download_common import AuthError, DownloadError, json_digest
from neognss_observatory.download_http import HTTP
from neognss_observatory.download_inventory_cache import InventoryCache
from neognss_observatory.download_plan import (
    load_plan,
    make_plan,
    read_config,
    reusable_candidate,
    summarize,
)
from neognss_observatory.download_plan_io import write_plan
from neognss_observatory.download_transfer import fetch, read_records, verify

console = Console(stderr=True)


class ProductCounts(DownloadColumn):
    def render(self, task):
        if task.description == "Files":
            return Text(f"{int(task.completed)}/{int(task.total)} files")
        return super().render(task)


class ProductSpeed(TransferSpeedColumn):
    def render(self, task):
        return Text("") if task.description == "Files" else super().render(task)


@contextlib.contextmanager
def errors():
    try:
        yield
    except AuthError as exc:
        console.print(str(exc), markup=False)
        raise click.exceptions.Exit(3) from exc
    except (DownloadError, OSError, sqlite3.Error) as exc:
        console.print(str(exc), markup=False)
        raise click.exceptions.Exit(1) from exc
    except KeyboardInterrupt as exc:
        console.print("Interrupted; partial transfers are retained.")
        raise click.exceptions.Exit(130) from exc


@click.group()
def cli():
    """Inventory and download CDDIS products using Earthdata netrc credentials."""


@cli.command("plan")
@click.option("--config", "--profile", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--start", type=click.DateTime(formats=["%Y-%m-%d"]))
@click.option("--end", type=click.DateTime(formats=["%Y-%m-%d"]))
@click.option("--netrc", "netrc_path", type=click.Path(path_type=Path), default="~/.netrc", show_default=True)
@click.option("--output", type=click.Path(path_type=Path), required=True, help="Write an atomic JSON Lines plan (.jsonl).")
@click.option("--root", type=click.Path(path_type=Path), default="work/products", show_default=True)
@click.option("--refresh-inventory", is_flag=True, help="Discard this output's unfinished inventory checkpoint and query again.")
def plan_command(config, start, end, netrc_path, output, root, refresh_inventory):
    """Query live listings and freeze a plan; product bodies are not downloaded."""
    try:
        settings, config_hash = read_config(config)
    except DownloadError as exc:
        raise click.UsageError(str(exc)) from exc
    if start:
        settings["start"] = start.date().isoformat()
    if end:
        settings["end"] = end.date().isoformat()
    if settings.get("end", settings["start"]) < settings["start"]:
        raise click.UsageError("end precedes start")
    if settings["start"] < "1980-01-06":
        raise click.UsageError("start must be on or after 1980-01-06")
    identity = json_digest({"cache_schema": 1, "config_sha512": config_hash, "resolved_config": settings})
    with (
        errors(),
        InventoryCache(str(output) + ".inventory.sqlite", identity, refresh_inventory) as checkpoint,
        contextlib.closing(HTTP(netrc_path, settings["network"])) as http,
    ):
        records, _ = read_records(root)
        with Progress(
            TextColumn("{task.description}"),
            BarColumn(),
            TextColumn("{task.fields[detail]}"),
            console=console,
            refresh_per_second=4,
            disable=not console.is_terminal,
        ) as progress:
            inventory_task = progress.add_task("Inventory", total=None, detail="Discovering date range")
            task = progress.add_task("Checksum snapshot", total=None, visible=False, detail="")

            def inventory_update(state):
                detail = (
                    f"{state['completed']}/{state['total']} weeks | {state['selected']} selected | "
                    f"{state['pending']} need download | {state['reusable']} reusable | {state['directories']} directories | "
                    f"{state['cached']} cached"
                )
                progress.update(inventory_task, total=state["total"], completed=state["completed"], detail=detail)
                if not console.is_terminal:
                    console.print(f"Weeks inventoried: {detail}", markup=False, soft_wrap=True)

            def checksum_update(done, total):
                progress.update(task, total=total, completed=done, visible=True, detail=f"{decimal(done)} / {decimal(total)}")

            result = make_plan(
                settings,
                config_hash,
                http,
                checksum_progress=checksum_update,
                inventory_progress=inventory_update,
                records=records,
                root=root,
                checkpoint=checkpoint,
            )
        write_plan(output, result)
        checkpoint.complete()
        reusable = {
            item["path"]: records[item["path"]]
            for item in result["files"]
            if reusable_candidate(item, records.get(item["path"], {}), root)
        }
        click.echo(json.dumps(summarize(result, reusable), indent=2))
        console.print(f"Plan saved: {output}", markup=False)
        # A successfully generated plan may intentionally contain gaps.


@cli.command("fetch")
@click.option("--plan", "plan_path", type=click.Path(exists=True, dir_okay=False, path_type=Path), required=True)
@click.option("--root", type=click.Path(path_type=Path), default="work/products", show_default=True)
@click.option("--netrc", "netrc_path", type=click.Path(path_type=Path), default="~/.netrc", show_default=True)
def fetch_command(plan_path, root, netrc_path):
    """Download the frozen plan; repeat to recover interruptions or repair damage."""
    with errors():
        plan = load_plan(plan_path)
        with contextlib.closing(HTTP(netrc_path, plan["network"])) as http:
            with Progress(
                TextColumn("{task.description}"),
                BarColumn(),
                ProductCounts(),
                ProductSpeed(),
                TimeRemainingColumn(),
                console=console,
                refresh_per_second=4,
                disable=not console.is_terminal,
            ) as progress:
                result, code = fetch(plan, root, http, progress)
        click.echo(json.dumps(result, indent=2))
        raise click.exceptions.Exit(code)


@cli.command("status")
@click.option("--root", type=click.Path(exists=True, file_okay=False, path_type=Path), default="work/products", show_default=True)
@click.option("--plan", "plan_path", type=click.Path(exists=True, dir_okay=False, path_type=Path))
def status_command(root, plan_path):
    """Report recorded state offline; use verify to check bytes on disk."""
    with errors():
        records, plans = read_records(root)
        plan = load_plan(plan_path) if plan_path else plans[-1] if plans else None
        if not plan:
            raise DownloadError("No recorded plan; supply --plan")
        result = summarize(plan, records)
        result["file_errors"] = {path: item.get("error") for path, item in records.items() if item["status"] != "complete"}
        result["missing_inputs"] = [slot for slot in plan["slots"] if slot.get("issue")]
        click.echo(json.dumps(result, indent=2))


@cli.command("verify")
@click.option("--root", type=click.Path(exists=True, file_okay=False, path_type=Path), default="work/products", show_default=True)
def verify_command(root):
    """Recheck recorded files offline, including compression and local SHA-512."""
    with errors():
        if not (root / "manifest.sqlite").exists():
            raise DownloadError("No manifest to verify")
        failures = verify(root, lambda message: console.print(message, markup=False, soft_wrap=True))
        click.echo(json.dumps({"failed_verifications": failures}))
        raise click.exceptions.Exit(1 if failures else 0)


if __name__ == "__main__":
    cli()
