# SPDX-License-Identifier: GPL-3.0-only
"""Bundled map resources and content identity."""

import functools
import hashlib
from contextlib import nullcontext
from importlib.resources import as_file, files


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
