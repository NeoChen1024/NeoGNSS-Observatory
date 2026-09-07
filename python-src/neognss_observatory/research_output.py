# SPDX-License-Identifier: GPL-3.0-only
"""Small shared publication helper for disposable research runs."""

import functools
import json
import uuid
from pathlib import Path

import click


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def staged_output(function):
    """Keep incomplete runs out of the final path; preserve replaced runs."""

    @functools.wraps(function)
    def wrapped(*args, **kwargs):
        overwrite = kwargs.pop("overwrite", False)
        target = Path(kwargs["output"]).absolute()
        if target.resolve() in (Path.cwd().resolve(), *Path.cwd().resolve().parents):
            raise click.ClickException("Output must not be the workspace or one of its parent directories")
        if target.is_symlink():
            raise click.ClickException("Output must not be a symlink")
        for key, value in kwargs.items():
            if key == "output":
                continue
            for source in value if isinstance(value, (tuple, list)) else (value,):
                if isinstance(source, Path) and (
                    source.resolve().is_relative_to(target.resolve())
                    or (source.is_dir() and target.resolve().is_relative_to(source.resolve()))
                ):
                    raise click.ClickException("Input and output paths must not contain each other")
        if target.exists() and (not overwrite or not target.is_dir()):
            raise click.ClickException("Output exists; use --overwrite to replace a research run")
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(f".{target.name}.partial-{uuid.uuid4().hex}")
        kwargs["output"] = partial
        try:
            result = function(*args, **kwargs)
        except BaseException:
            if partial.exists():
                click.echo(f"Incomplete output retained at {partial}", err=True)
            raise
        backup = None
        if target.exists():
            if not overwrite or target.is_symlink() or not target.is_dir():
                raise click.ClickException(f"Output appeared during processing; result retained at {partial}")
            backup = target.with_name(f"{target.name}.backup-{uuid.uuid4().hex}")
            target.rename(backup)
        try:
            partial.rename(target)
        except BaseException:
            if backup is not None:
                backup.rename(target)
            raise
        if backup is not None:
            click.echo(f"Previous output retained at {backup}", err=True)
        click.echo(f"Output: {target}", err=True)
        return result

    return wrapped
