# SPDX-License-Identifier: GPL-3.0-only
"""Select expanded recordings without requiring a QA or reconstruction product."""

import re


def recordings(root, protocol="ubx", recursive=True):
    paths = []
    for path in root.rglob("*") if recursive else root.iterdir():
        parts = path.relative_to(root).parts
        if "unassigned" in parts or any(part.startswith(".") for part in parts):
            continue
        suffix = path.suffix.lower()
        selected = suffix == ".ubx" or protocol == "sbf" and (suffix == ".sbf" or re.fullmatch(r"\.\d{2}_", suffix))
        if selected and path.is_file():
            paths.append(path)
    if not paths:
        raise ValueError(f"No expanded {protocol.upper()} recordings; compressed archives are not processing inputs")
    # Filenames establish traversal only. Time is always decoded from payload.
    return sorted(paths)
