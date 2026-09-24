# SPDX-License-Identifier: GPL-3.0-only
"""Bundled map resources and content identity."""

import functools
import hashlib
import zipfile
from contextlib import nullcontext
from importlib.resources import as_file, files
from io import BytesIO

import shapefile


def coastline_parts(path):
    """Read bundled coastline geometry without selecting a Matplotlib backend."""
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        members = {suffix: next(n for n in names if n.endswith("." + suffix)) for suffix in ("shp", "shx", "dbf")}
        with shapefile.Reader(**{k: BytesIO(archive.read(n)) for k, n in members.items()}) as reader:
            for shape in reader.iterShapes():
                boundaries = list(shape.parts) + [len(shape.points)]
                for begin, finish in zip(boundaries, boundaries[1:]):
                    yield shape.points[begin:finish]


def with_coastline(function):
    """Keep any extracted resource alive until all rendering workers finish."""

    @functools.wraps(function)
    def wrapped(*args, **kwargs):
        path = kwargs.get("coastline")
        context = (
            nullcontext(path) if path is not None else as_file(files("neognss_observatory.data").joinpath("ne_10m_coastline.zip"))
        )
        with context as coastline:
            kwargs["coastline"] = coastline
            return function(*args, **kwargs)

    return wrapped


def coastline_sha512(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha512").hexdigest()
