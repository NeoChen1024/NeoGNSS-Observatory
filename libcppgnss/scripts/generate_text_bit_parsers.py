#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Generate owned, hierarchical RTCM3/NMEA messages from pinned schemas.

RTCM fields retain encoded integer values and expose the upstream scale as
metadata. No RINEX identity, sentinel normalization or epoch assembly occurs.
Unsupported schema constructs produce explicit errors, never partial success.
"""
import copy
import json
import re
import sys
from pathlib import Path

import click

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "contrib" / name / "src") for name in ("pyrtcm", "pynmeagps")]
from pynmeagps.nmeatypes_get import NMEA_PAYLOADS_GET
from pynmeagps.nmeatypes_get_prop import NMEA_PAYLOADS_GET_PROP
from pyrtcm.rtcmtypes_core import RTCM_DATA_FIELDS, SSR_COEFF
from pyrtcm.rtcmtypes_get import RTCM_PAYLOADS_GET
from pyrtcm.rtcmtypes_get_igs import RTCM_PAYLOADS_GET_IGS
from pyrtcm.rtcmtypes_get_msm import RTCM_PAYLOADS_GET_MSM

NOTICE = "// Generated from semuconsulting schemas (BSD-3-Clause); do not edit.\n// Copyright (c) semuadmin (Steve Smith). See contrib/pyrtcm/LICENSE and contrib/pynmeagps/LICENSE.\n"


def ident(name):
    result = re.sub(r"\W", "_", name)
    if result[0].isdigit() or result in {
        "class",
        "operator",
        "typeid",
        "default",
        "delete",
        "new",
        "long",
        "int",
        "char",
        "protocol",
        "message_id",
        "message_name",
        "matches",
        "decode_payload",
        "dump",
        "address",
        "delimiter",
    }:
        result = "field_" + result
    return result


class Unsupported(Exception):
    pass


class Generator:
    def __init__(self, protocol):
        self.protocol = protocol
        self.serial = 0

    def sequence(self, fields, target="message", scope=None):
        if not isinstance(fields, dict):
            # Pinned pyrtcm MSM2 contains a set in place of this one-field dict.
            if fields == {"DF420", "Half-cycle ambiguity indicator"}:
                fields = {"DF420": "Half-cycle ambiguity indicator"}
            else:
                raise Unsupported("Undefined group schema")
        scope = dict(scope or {})
        members, code, dump = [], [], []
        for key, spec in fields.items():
            name = ident(key)
            value = f"{target}.{name}"
            if isinstance(spec, tuple):
                repeat, children = spec
                self.serial += 1
                idx = self.serial
                var = f"item{idx}"
                child_members, child_code, child_dump = self.sequence(children, var, scope)
                if not child_members:
                    continue
                group = name + "Entry"
                members.append(f"struct {group} {{ {' '.join(child_members)} }};")
                if isinstance(repeat, tuple):
                    ref, expected = repeat
                    if ref not in scope:
                        # RTCM 1230 declares its four optional biases by bits.
                        if ref.startswith("DF422_") and "DF422" in scope:
                            expression = f"(({scope['DF422']} >> {4-int(ref[-1])}) & 1)"
                        else:
                            raise Unsupported(f"Unknown condition {ref}")
                    else:
                        expression = scope[ref]
                    members.append(f"std::optional<{group}> {name};")
                    code += [f"if ({expression} == {expected}) {{ auto &{var}={value}.emplace();", *child_code, "}"]
                    dump += [
                        f"if ({value}) {{ out.begin({json.dumps(key)}); const auto &{var}=*{value};",
                        *child_dump,
                        "out.end(); }",
                    ]
                else:
                    if isinstance(repeat, int):
                        count = str(repeat)
                    elif self.protocol == "rtcm3" and repeat in {"NSat", "NCell"}:
                        count = "nsat" if repeat == "NSat" else "ncell"
                    elif self.protocol == "rtcm3" and repeat in {"_NHarmCoeffC", "_NHarmCoeffS"}:
                        bases = next((pair for pair in SSR_COEFF.values() if all(k in scope for k in pair)), None)
                        if bases is None:
                            raise Unsupported("Missing harmonic degree/order")
                        n, m = (f"({scope[k]}+1)" for k in bases)
                        code.append(f'if ({m}>{n}) throw detail::WireError{{cursor.bit/8,"Harmonic order exceeds degree"}};')
                        count = f"(({n}+1)*({n}+2)-({n}-{m})*({n}-{m}+1))/2"
                        if repeat == "_NHarmCoeffS":
                            count = f"({count})-({n}+1)"
                    elif repeat == "None" and self.protocol == "nmea":
                        if any(isinstance(v, tuple) for v in children.values()):
                            raise Unsupported("Nested variable-width NMEA group")
                        count = f"cursor.remaining()/{len(children)}"
                    else:
                        ref = repeat.split("+")[0]
                        if ref not in scope:
                            raise Unsupported(f"Unknown repetition {repeat}")
                        count = scope[ref]
                        if self.protocol == "nmea":
                            count = f"cursor.count({count})"
                        elif ref == "IDF035":
                            count = f"({count}+1)"
                    if self.protocol == "rtcm3":
                        count = f"cursor.count({count})"
                    members.append(f"std::vector<{group}> {name};")
                    code += [f"{value}.resize({count}); for(auto &{var}:{value}) {{", *child_code, "}"]
                    dump += [
                        f'out.begin({json.dumps(key)}, {{}}, true); for(const auto &{var}:{value}) {{ out.begin("");',
                        *child_dump,
                        "out.end(); } out.end();",
                    ]
                continue
            if self.protocol == "rtcm3":
                if key not in RTCM_DATA_FIELDS:
                    raise Unsupported(f"Unknown field {key}")
                kind, width, scale, description = RTCM_DATA_FIELDS[key]
                if kind in {"PRN", "CPR", "CSG"}:
                    continue  # Python-derived labels, not fields on the wire.
                if kind not in {"INT", "UINT", "BIT", "BITX", "SNT", "CHA", "STR"} or width > 64:
                    raise Unsupported(f"Unsupported field type {key}")
                if width == 0 and key != "DF396":
                    raise Unsupported(f"Unspecified field width {key}")
                typ = "int64_t" if kind == "INT" else "uint64_t"
                read_width = "nsat*nsig" if key == "DF396" else str(width)
                members += [
                    f"{typ} {name} = 0;",
                    f"static constexpr double {name}_scale = {scale!r};",
                    f"static constexpr std::string_view {name}_encoding = {json.dumps(kind)};",
                ]
                code.append(f"{value}=cursor.{'s' if kind=='INT' else 'u'}({read_width});")
                if key in {"DF394", "DF395", "DF396"}:
                    code.append(f"{'nsat' if key=='DF394' else 'nsig' if key=='DF395' else 'ncell'}=std::popcount({value});")
            else:
                if spec == "HX":
                    typ, read = "HexInteger", "cursor.hex()"
                elif spec == "IN":
                    typ, read = "int64_t", "cursor.number<int64_t>()"
                elif spec in {"DE", "LA", "LN"}:
                    typ, read = "double", "cursor.number<double>()"
                elif spec in {"ST", "TM", "DT", "DTL", "DM", "CH", "LAD", "LND", "QS"}:
                    typ, read = "std::string", "cursor.text()"
                else:
                    raise Unsupported(f"Unknown NMEA type {spec}")
                members.append(f"std::optional<{typ}> {name};")
                code.append(f"{value}={read};")
            scope[key] = value
            dump.append(f"out.field({json.dumps(key)}, detail::show({value}));")
        return members, code, dump


def generate(protocol, schemas, output):
    ns = "RTCM3" if protocol == "rtcm3" else "NMEA"
    enum = "Rtcm3MessageId" if protocol == "rtcm3" else "NmeaMessageId"
    header = [NOTICE, "#pragma once\n#include <cppgnss/wire_typed.hpp>"]
    declarations, implementations, cases, ids, unsupported = [], [], [], [], []
    for ordinal, (key, fields) in enumerate(sorted(schemas.items())):
        if protocol == "rtcm3":
            numeric = int(key.split("_")[0])
            subtype = int(key.split("_")[1]) if "_" in key else -1
            name = (
                f"{['GPS','GLO','GAL','SBAS','QZS','BDS','NAVIC'][numeric//10-107]}_MSM{numeric%10}"
                if 1071 <= numeric <= 1137 and 1 <= numeric % 10 <= 7
                else "MT" + key
            )
            match = f"detail::rtcm_matches(frame,{numeric},{subtype})"
            # Enum identifies wire message numbers; subtypes remain type traits.
            if subtype < 0 or f"MT{numeric} = {numeric}" not in ids:
                entry = f"{name if subtype<0 else 'MT'+str(numeric)} = {numeric}"
                if entry not in ids:
                    ids.append(entry)
            enum_value = name if subtype < 0 else f"MT{numeric}"
        else:
            name = ident(key)
            ids.append(f"{name} = {ordinal}")
            enum_value = name
            match = f"detail::nmea_identity(frame)=={json.dumps(key)}"
        g = Generator(protocol)
        error = None
        try:
            members, code, dump = g.sequence(copy.deepcopy(fields))
        except Unsupported as exc:
            members, code, dump = [], [], []
            error = str(exc)
            unsupported.append(f"{key}: {error}")
        extra = "std::string address; char delimiter='$';" if protocol == "nmea" else ""
        declarations.append(
            f"struct {name} {{ static constexpr auto protocol=Protocol::{protocol}; static constexpr auto message_id={enum}::{enum_value}; static constexpr std::string_view message_name={json.dumps(key)}; static bool matches(const FrameView &frame) {{return {match};}} {extra} {' '.join(members)} static ParseResult<{name}> decode_payload(const FrameView &frame); std::string dump() const; }};"
        )
        body = (
            f"(void)frame; return ParseError{{ParseErrorCode::UNSUPPORTED_LAYOUT,{{}},{json.dumps(error)}}};"
            if error
            else (
                f"detail::{'BitCursor' if protocol=='rtcm3' else 'TextCursor'} cursor{{frame.payload}}; try {{ {name} message; "
                + (
                    "[[maybe_unused]] size_t nsat=0,nsig=0,ncell=0;"
                    if protocol == "rtcm3"
                    else "const auto &h=std::get<NmeaHeader>(frame.header); message.address=h.address; message.delimiter=h.delimiter;"
                )
                + ("cursor.brackets=true;" if protocol == "nmea" and key == "SSNSNC" else "")
                + "\n".join(code)
                + f"return ParsedMessage<{name}>{{std::move(message),{'(cursor.bit+7)/8' if protocol=='rtcm3' else 'cursor.offset'}}}; }} catch(const detail::WireError &e) {{ return ParseError{{ParseErrorCode::INVALID_PAYLOAD,e.offset,e.detail}}; }}"
            )
        )
        implementations.append(f"ParseResult<{name}> {name}::decode_payload(const FrameView &frame) {{ {body} }}")
        implementations.append(
            f'std::string {name}::dump() const {{ const auto &message=*this; (void)message; detail::TextDump out; out.text="({ns}-{key}";'
            + ('out.field("address",detail::quoted(address));' if protocol == "nmea" else "")
            + "\n".join(dump)
            + "return std::move(out).finish(); }"
        )
        condition = f"identity=={json.dumps(key)}" if protocol == "nmea" else f"{ns}::{name}::matches(frame)"
        cases.append(f"if({condition}) return dump_parsed(frame,parse<{ns}::{name}>(frame));")
    header += [f"namespace cppgnss {{ enum class {enum} : uint16_t {{ {','.join(ids)} }}; namespace {ns} {{", *declarations, "}}"]
    (output / f"{protocol}_gen.hpp").write_text("\n".join(header) + "\n")
    source = [
        NOTICE,
        f"#include <cppgnss/{protocol}_gen.hpp>",
        f"namespace cppgnss::{ns} {{",
        *implementations,
        "}",
        f"namespace cppgnss::detail {{ std::string dump_{protocol}(const FrameView &frame) {{",
        "auto identity=nmea_identity(frame);" if protocol == "nmea" else "",
        *cases,
        "return dump_raw(frame); } }",
    ]
    (output / f"{protocol}_gen.cpp").write_text("\n".join(source) + "\n")
    (output / f"{protocol}_unsupported.txt").write_text("\n".join(unsupported) + "\n")
    print(f"{protocol}: {len(schemas)-len(unsupported)} typed layouts, {len(unsupported)} explicitly unsupported")


@click.command()
@click.option("--output-dir", type=click.Path(path_type=Path), required=True)
def main(output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    generate("rtcm3", RTCM_PAYLOADS_GET | RTCM_PAYLOADS_GET_MSM | RTCM_PAYLOADS_GET_IGS, output_dir)
    generate("nmea", NMEA_PAYLOADS_GET | NMEA_PAYLOADS_GET_PROP, output_dir)


if __name__ == "__main__":
    main()
