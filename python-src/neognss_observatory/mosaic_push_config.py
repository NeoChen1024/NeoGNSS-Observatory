# SPDX-License-Identifier: GPL-3.0-only
"""Configuration and receiver-compatible date paths for mosaic push."""

import json
import re
from datetime import date
from pathlib import Path, PurePosixPath


class PushError(Exception):
    """An actionable configuration, storage, or transfer failure."""


def object_fields(value, allowed, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or set(required) - set(value):
        raise PushError(f"Expected an object with fields {', '.join(allowed)}; required: {', '.join(required)}")


def text_field(value, label, empty=False):
    if not isinstance(value, str) or (not value and not empty) or any(c in value for c in "\r\n\x00"):
        raise PushError(f"Invalid {label}")
    return value


def integer(value, label, low, high):
    if type(value) is not int or not low <= value <= high:
        raise PushError(f"{label} must be an integer in {low}..{high}")
    return value


def remote_path(value):
    text_field(value, "remote path")
    if ".." in PurePosixPath(value).parts or "\\" in value:
        raise PushError("Remote paths must not contain '..' or backslashes")
    return value


def expand_path(template, day):
    """Use setFTPPushSBF substitutions, not unrestricted strftime."""
    substitutions = {
        "Y": f"{day.year:04d}",
        "y": f"{day.year % 100:02d}",
        "m": f"{day.month:02d}",
        "d": f"{day.day:02d}",
        "j": f"{(day - date(day.year, 1, 1)).days + 1:03d}",
        "%": "%",
    }
    result = []
    index = 0
    while index < len(template):
        char = template[index]
        if char == "%":
            index += 1
            if index == len(template) or template[index] not in substitutions:
                raise PushError("destination.path supports only %Y, %y, %m, %d, %j and %%")
            char = substitutions[template[index]]
        result.append(char)
        index += 1
    return remote_path("".join(result))


def read_config(path):
    try:

        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise PushError("Duplicate JSON configuration field")
                result[key] = value
            return result

        config = json.loads(path.read_text(), object_pairs_hook=unique_object)
    except (OSError, ValueError) as exc:
        raise PushError("Cannot read a valid JSON configuration") from exc
    object_fields(
        config,
        ("archive_root", "source", "destination", "schedule", "network", "compression"),
        ("archive_root", "source"),
    )
    root = Path(text_field(config["archive_root"], "archive_root")).expanduser()
    if not root.is_absolute():
        raise PushError("archive_root must be absolute")
    config["archive_root"] = root.resolve()
    if config.get("destination") is None:
        config["destination"] = {"enabled": False}
    for name in ("source", "destination"):
        endpoint = config[name]
        fields = ("host", "port", "username", "password", "path")
        extra = ("station", "year_base") if name == "source" else ("enabled", "tls", "ca_file", "verify_tls")
        object_fields(endpoint, fields + extra)
        if name == "destination":
            endpoint.setdefault("enabled", True)
            if type(endpoint["enabled"]) is not bool:
                raise PushError("destination.enabled must be a boolean")
            if not endpoint["enabled"]:
                continue
            object_fields(endpoint, fields + extra, fields)
        else:
            object_fields(endpoint, fields + extra, ("host", "path"))
            endpoint.setdefault("port", 21)
            endpoint.setdefault("username", "anonymous")
            endpoint.setdefault("password", "")
        for field in ("host", "username", "password"):
            text_field(endpoint[field], f"{name}.{field}", empty=field == "password")
        integer(endpoint["port"], f"{name}.port", 1, 65535)
        remote_path(endpoint["path"])
    source = config["source"]
    source.setdefault("station", "bee_")
    if not isinstance(source["station"], str) or not re.fullmatch(r"[a-z0-9_]{4}", source["station"]):
        raise PushError("source.station must contain four lowercase ASCII letters, digits or underscores")
    source.setdefault("year_base", 2000)
    integer(source["year_base"], "source.year_base", 1900, 9900)
    if source["year_base"] % 100:
        raise PushError("source.year_base must be a century, such as 2000")
    destination = config["destination"]
    if destination["enabled"]:
        destination.setdefault("verify_tls", True)
        if type(destination["verify_tls"]) is not bool:
            raise PushError("destination.verify_tls must be a boolean")
        destination.setdefault("tls", "explicit")
        if destination["tls"] not in ("explicit", "implicit"):
            raise PushError("destination.tls must be explicit or implicit")
        if destination["verify_tls"] and destination.get("ca_file") is not None:
            ca = Path(text_field(destination["ca_file"], "destination.ca_file")).expanduser()
            if not ca.is_absolute() or not ca.is_file():
                raise PushError("destination.ca_file must be an existing absolute file path")
            destination["ca_file"] = str(ca)
        expand_path(destination["path"], date(2026, 9, 15))
    defaults = {
        "schedule": {"daily_utc": "00:10", "retry_seconds": 600},
        "network": {
            "connect_timeout_seconds": 30,
            "stall_timeout_seconds": 300,
            "completion_timeout_seconds": 300,
            "tcp_keepalive_idle_seconds": 60,
            "tcp_keepalive_interval_seconds": 30,
            "tcp_keepalive_probes": 5,
        },
        "compression": {"preset": 6, "threads": 2, "memory_limit_mib": 512},
    }
    for section, values in defaults.items():
        supplied = config.setdefault(section, {})
        object_fields(supplied, values)
        for key, value in values.items():
            supplied.setdefault(key, value)
    schedule = config["schedule"]
    if not isinstance(schedule["daily_utc"], str) or not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", schedule["daily_utc"]):
        raise PushError("schedule.daily_utc must be HH:MM in UTC")
    integer(schedule["retry_seconds"], "schedule.retry_seconds", 1, 86400)
    for key in defaults["network"]:
        upper = 127 if key == "tcp_keepalive_probes" else 32767 if key.startswith("tcp_keepalive_") else 86400
        integer(config["network"][key], f"network.{key}", 1, upper)
    compression = config["compression"]
    integer(compression["preset"], "compression.preset", 0, 9)
    integer(compression["threads"], "compression.threads", 1, 64)
    integer(compression["memory_limit_mib"], "compression.memory_limit_mib", 32, 1048576)
    return config
