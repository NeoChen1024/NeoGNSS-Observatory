#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the complete SBF descriptor tree from the pinned pysbf2 schema.

Schema order is significant. Nested groups, bitfields, conditional groups and
sub-block length padding remain structural rather than flattened byte offsets.
"""

import json
import sys
import types
from pathlib import Path

import click


def field(name, definition):
    quote = json.dumps
    kind, typ, width, scale, reference, count, conditions, children = "scalar", "U", 0, 1, "", 0, [], []
    if isinstance(definition, tuple):
        repeat, members = definition
        children = [field(k, v) for k, v in members.items()]
        if isinstance(repeat, str) and repeat.startswith("X") and repeat[1:].isdigit():
            kind, width = "bits", int(repeat[1:])
        elif isinstance(repeat, tuple):
            kind, reference = "optional", repeat[0]
            conditions = repeat[1] if isinstance(repeat[1], list) else [repeat[1]]
        else:
            kind = "repeat"
            if isinstance(repeat, int):
                count = repeat
            else:
                reference = repeat
    elif definition in ("SBLength", "SB1Length", "SB2Length"):
        kind, reference = "padding", definition
    else:
        if isinstance(definition, list):
            definition, scale = definition
        typ, width = definition[0], int(definition[1:])
        if typ not in "UIFXCPV":
            raise ValueError(f"Unsupported SBF type: {definition}")
    return (
        "{"
        + f"{quote(name)},Kind::{kind},'{typ}',{width},{scale},{quote(reference)},{count},"
        + "{"
        + ",".join(map(str, conditions))
        + "},{"
        + ",".join(children)
        + "}}"
    )


@click.command()
@click.option("--output", type=click.Path(path_type=Path), required=True)
def cli(output):
    """Generate all known block schemas, including explicit empty definitions."""
    source = Path(__file__).resolve().parents[2] / "contrib/pysbf2/src/pysbf2"
    # Read definition modules only: pysbf2 is not a runtime dependency.
    package = types.ModuleType("pysbf2")
    package.__path__ = [str(source)]
    sys.modules["pysbf2"] = package
    from pysbf2.sbftypes_blocks import SBF_BLOCKS
    from pysbf2.sbftypes_core import SBF_MSGIDS

    ids = {value[0]: key for key, value in SBF_MSGIDS.items()}
    blocks = [
        "{" + f"{ids[name]},{json.dumps(name)}," + "{" + ",".join(field(k, v) for k, v in definition.items()) + "}}"
        for name, definition in SBF_BLOCKS.items()
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "// Generated from pysbf2 (BSD-3-Clause); do not edit.\n"
        "// Copyright (c) 2025 semuadmin (Steve Smith). See contrib/pysbf2/LICENSE.\n"
        "#include <cppgnss/sbf.hpp>\nnamespace cppgnss::SBF {\n"
        "const std::vector<Schema>& schemas() { static const std::vector<Schema> value = {\n"
        + ",\n".join(blocks)
        + "\n}; return value; }\n}\n"
    )
    click.echo(f"Generated {len(blocks)} SBF schemas ({sum(bool(v) for v in SBF_BLOCKS.values())} defined payloads).")


if __name__ == "__main__":
    cli()
