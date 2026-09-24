#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the complete SBF descriptor tree from the pinned pysbf2 schema.

Schema order is significant. Nested groups, bitfields, conditional groups and
sub-block length padding remain structural rather than flattened byte offsets.
"""

import copy
import json
import re
import sys
import types
from pathlib import Path

import click


def enum_name(name):
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)).upper()


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


GROUPS = {
    "measurement": "MEASUREMENT",
    "navigation_page": "NAVIGATION_PAGE",
    "gps_navigation": "GPS_DECODED_MESSAGE",
    "glonass_navigation": "GLONASS_DECODED_MESSAGE",
    "galileo_navigation": "GALILEO_DECODED_MESSAGE",
    "beidou_navigation": "BEIDOU_DECODED_MESSAGE",
    "navic_navigation": "NAVIC_DECODED_MESSAGE",
    "qzss_navigation": "QZSS_DECODED_MESSAGE",
    "sbas_navigation": "SBAS_L1_DECODED_MESSAGE",
    "pvt": "GNSS_POSITION_VELOCITY_TIME",
    "attitude": "GNSS_ATTITUDE",
    "receiver_time": "RECEIVER_TIME",
    "external_event": "EXTERNAL_EVENT",
    "differential_correction": "DIFFERENTIAL_CORRECTION",
    "lband": "LBAND_DEMODULATOR",
    "status": "STATUS",
    "miscellaneous": "MISCELLANEOUS",
}
NOTICE = "// Generated from pysbf2 (BSD-3-Clause); do not edit.\n// Copyright (c) 2025 semuadmin (Steve Smith). See contrib/pysbf2/LICENSE.\n"


def identifier(name):
    value = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    if not value or value[0].isdigit() or value.startswith("_"):
        value = "field_" + value
    return value


def scalar_type(definition):
    value, scale = definition if isinstance(definition, list) else (definition, 1)
    typ, width = value[0], int(value[1:])
    if typ in "XCV":
        return "std::vector<uint8_t>", typ, width, scale
    if typ == "P":
        return "", typ, width, scale
    if typ == "F" or scale != 1:
        return "double", typ, width, scale
    if width > 8:
        return "WideInteger", typ, width, scale
    bits = next(n for n in (8, 16, 32, 64) if width * 8 <= n)
    return f"{'int' if typ == 'I' else 'uint'}{bits}_t", typ, width, scale


def members(definition, prefix=""):
    declarations = {}
    for name, value in definition.items():
        member = identifier(name)
        if value in ("SBLength", "SB1Length", "SB2Length"):
            continue
        if isinstance(value, tuple):
            repeat, children = value
            if isinstance(repeat, str) and repeat.startswith("X") and repeat[1:].isdigit():
                fields = "\n".join("uint64_t " + identifier(n) + "{};" for n in children)
                declarations[member] = f"struct {prefix}{member}_bits {{\n{fields}\n}} {member};"
            else:
                kind = "std::optional" if isinstance(repeat, tuple) else "std::vector"
                item = prefix + member + "_item"
                declarations[member] = f"struct {item} {{\n{members(children, item + '_')}\n}};\n{kind}<{item}> {member};"
        else:
            typ, _, _, _ = scalar_type(value)
            if typ:
                declarations[member] = f"{typ} {member}{{}};"
    return "\n".join(declarations.values())


class Parser:
    def __init__(self, message):
        self.message = message
        self.lines = []
        self.serial = 0

    def add(self, line):
        self.lines.append(line)

    def ref(self, name, env):
        if name == "__revision":
            return "frame.revision()"
        parts = name.split("+")
        depth = int(parts[1]) if len(parts) > 1 else 0
        expression = env[(parts[0], depth)]
        return f"detail::reference({expression}, {json.dumps(parts[0])}, suffix{depth})"

    def sequence(self, definition, obj, env, depth=0, base="base0"):
        for name, value in definition.items():
            member = obj + "." + identifier(name)
            if value in ("SBLength", "SB1Length", "SB2Length"):
                self.add(f"cursor.padding({base}, {self.ref(value, env)});")
                continue
            if isinstance(value, tuple):
                repeat, children = value
                self.serial += 1
                unique = self.serial
                if isinstance(repeat, str) and repeat.startswith("X") and repeat[1:].isdigit():
                    self.add("{")
                    self.add(f"auto bits{unique} = cursor.take({int(repeat[1:])});")
                    self.add(f"detail::Group<Sink> group{unique}(sink, {json.dumps(name)});")
                    offset = 0
                    for bitname, bitdef in children.items():
                        width = int(bitdef[1:])
                        target = member + "." + identifier(bitname)
                        self.add(f"{target} = detail::bits(bits{unique}, {offset}, {width});")
                        self.add(f"detail::emit(sink, {json.dumps(bitname)}, suffix{depth}, {target});")
                        env[(bitname, depth)] = target
                        offset += width
                    self.add("}")
                elif isinstance(repeat, tuple):
                    choices = repeat[1] if isinstance(repeat[1], list) else [repeat[1]]
                    reference = self.ref(repeat[0], env)
                    self.add(f"auto condition{unique} = {reference};")
                    condition = " || ".join(f"condition{unique} == uint64_t({n})" for n in choices)
                    self.add(f"if ({condition}) {{ {member}.emplace();")
                    self.add(f"detail::Group<Sink> group{unique}(sink, {json.dumps(name)});")
                    self.sequence(children, f"(*{member})", env.copy(), depth, base)
                    self.add("}")
                else:
                    self.add("{")
                    count = str(repeat) if isinstance(repeat, int) else self.ref(repeat, env)
                    if repeat == "RLMLength":
                        count = f"({count} == 160 ? 5 : 3)"
                    if self.message == "MeasExtra" and name == "group":
                        count = "detail::measextra_count(frame.payload.size(), message.N, message.SBLength, frame.revision())"
                    self.add(f"auto count{unique} = cursor.count({count});")
                    if self.message in ("MeasEpoch", "MeasExtra"):
                        # MeasEpoch counters are uint8; MeasExtra is bounded
                        # by verified sub-block capacity above.
                        self.add(f"{member}.reserve(count{unique});")
                    self.add(f"detail::Group<Sink> group{unique}(sink, {json.dumps(name)});")
                    # Do not reserve attacker-controlled counts before checking payload reads.
                    self.add(f"for(size_t i{unique}=0;i{unique}<count{unique};++i{unique}) {{")
                    self.add(f"auto &item{unique} = {member}.emplace_back();")
                    self.add(f'detail::Group<Sink> entry{unique}(sink, "", i{unique});')
                    self.add(f"[[maybe_unused]] auto base{unique} = cursor.offset;")
                    self.add(f"auto suffix{depth+1} = detail::suffix<Sink>(suffix{depth}, i{unique});")
                    self.sequence(children, f"item{unique}", env.copy(), depth + 1, f"base{unique}")
                    self.add("}")
                    self.add("}")
                continue
            typ, wiretype, width, scale = scalar_type(value)
            if wiretype == "P":
                self.add(f"cursor.take({width});")
                continue
            if wiretype == "V":
                expression = "cursor.remaining()"
            else:
                readtype = typ
                if scale != 1 and wiretype != "F":
                    readtype = "int64_t" if wiretype == "I" else "uint64_t"
                expression = f"cursor.scalar<{readtype}, {width}, {'true' if wiretype=='I' else 'false'}>()"
                if scale != 1:
                    expression = f"double({expression}) * {scale}"
            self.add(f"{member} = {expression};")
            self.add(f"detail::emit(sink, {json.dumps(name)}, suffix{depth}, {member});")
            env[(name, depth)] = member


# Message-specific display overrides belong here, never in logger dispatch.
# These affect text only; stored wire values are unchanged.
DUMP_FORMATS = {("xPPSOffset", "Offset"): "{:+.17g}"}


def dump_members(message, definition, obj="(*this)", path="", depth=0):
    lines = []
    for name, value in definition.items():
        member = obj + "." + identifier(name)
        key = path + name
        if value in ("SBLength", "SB1Length", "SB2Length"):
            continue
        if isinstance(value, tuple):
            repeat, children = value
            if isinstance(repeat, str) and repeat.startswith("X") and repeat[1:].isdigit():
                lines.append(f"out.begin({json.dumps(name)});")
                for bitname, bitdef in children.items():
                    target = member + "." + identifier(bitname)
                    expression = f"bool({target})" if int(bitdef[1:]) == 1 else target
                    lines.append(f'out.field({json.dumps(bitname)}, std::format("{{}}", {expression}));')
                lines.append("out.end();")
            elif isinstance(repeat, tuple):
                lines.append(f"if ({member}) {{ out.begin({json.dumps(name)});")
                lines.extend(dump_members(message, children, f"(*{member})", key + ".", depth + 1))
                lines.append("out.end(); }")
            else:
                lines.append(f"{{ out.begin({json.dumps(name)}, {{}}, true); size_t index{depth}=0;")
                lines.append(f'for(const auto &item{depth}: {member}) {{ out.begin("", index{depth}++);')
                lines.extend(dump_members(message, children, f"item{depth}", key + ".", depth + 1))
                lines.append("out.end(); } out.end(); }")
            continue
        typ, wire, _, _ = scalar_type(value)
        if not typ:
            continue
        if typ == "std::vector<uint8_t>":
            expression = f'"hex:" + detail::hex({member})'
        elif typ == "WideInteger":
            expression = f'std::string({member}.is_signed ? "signed" : "unsigned") + " little-endian hex:" + detail::hex({member}.little_endian)'
        else:
            pattern = DUMP_FORMATS.get((message, key), "{:.17g}" if typ == "double" else "{}")
            expression = f"std::format({json.dumps(pattern)}, {member})"
        lines.append(f"out.field({json.dumps(name)}, {expression});")
    return lines


def generate_group(group, blocks, ids):
    header = [NOTICE, "#pragma once\n#include <cppgnss/sbf_typed.hpp>\nnamespace cppgnss::SBF {"]
    source = [NOTICE, f"#include <cppgnss/sbf_{group}_gen.hpp>\nnamespace cppgnss::SBF {{"]
    descriptors = []
    for name, definition in blocks.items():
        header += [
            f"struct {name} {{\nstatic constexpr auto protocol = Protocol::sbf;\nstatic constexpr auto message_id = SbfMessageId::{enum_name(name)};\nstatic constexpr std::string_view message_name = {json.dumps(name)};\nstatic bool matches(const FrameView &frame) {{ return frame.id() == static_cast<uint16_t>(message_id); }}\n{members(definition)}\nstd::string dump() const;\nstatic ParseResult<{name}> decode_payload(const FrameView &frame);\n}};",
        ]
        parser = Parser(name)
        parser.sequence(definition, "message", {})
        navigation_supported = group == "navigation_page" and name not in ("GLORawCA", "NAVICRaw")
        maximum_revision = 3 if name == "MeasExtra" else 1 if name == "MeasEpoch" else 0 if navigation_supported else None
        revision_guard = (
            []
            if maximum_revision is None
            else [
                f'if (frame.revision() > {maximum_revision}) return ParseError{{ParseErrorCode::UNSUPPORTED_REVISION, {{}}, "Unsupported {name} revision"}};'
            ]
        )
        source += [
            f"ParseResult<{name}> {name}::decode_payload(const FrameView &frame) {{",
            *revision_guard,
            f"[[maybe_unused]] {name} message{{}}; detail::Cursor cursor{{frame.payload}}; [[maybe_unused]] detail::NoFields sink;",
            "[[maybe_unused]] const size_t base0=0; const std::string suffix0;",
            "try {",
            *[line.replace("Sink", "detail::NoFields") for line in parser.lines],
            "} catch(const detail::PayloadError &e) { return ParseError{ParseErrorCode::INVALID_PAYLOAD, cursor.offset, e.what()}; }",
        ]
        if not definition:
            source.append('return ParseError{ParseErrorCode::UNSUPPORTED_LAYOUT, {}, "Unsupported schema"};')
        else:
            if navigation_supported:
                source.append(
                    'if (cursor.offset != frame.payload.size()) return ParseError{ParseErrorCode::INVALID_PAYLOAD, cursor.offset, "Unexpected navigation payload length"};'
                )
            source.append(f"return ParsedMessage<{name}>{{std::move(message), cursor.offset}};")
        source += [
            "}",
            f"std::string {name}::dump() const {{ detail::TextDump out; out.text={json.dumps('(SBF '+name)};",
            *dump_members(name, definition),
            "return std::move(out).finish(); }",
        ]
        descriptors.append(
            "{" + f"{ids[name]},{json.dumps(name)}," + "{" + ",".join(field(k, v) for k, v in definition.items()) + "}}"
        )
    source += [
        f"const std::vector<Schema> &schemas_{group}() {{ static const std::vector<Schema> value = {{",
        ",\n".join(descriptors),
        "}; return value; }",
        "}",
    ]
    header += ["}"]
    return "\n".join(header) + "\n", "\n".join(source) + "\n"


@click.command()
@click.option("--output-dir", type=click.Path(path_type=Path), required=True)
def cli(output_dir):
    """Generate all known block schemas, including explicit empty definitions."""
    source = Path(__file__).resolve().parents[2] / "contrib/pysbf2/src/pysbf2"
    # Read definition modules only: pysbf2 is not a runtime dependency.
    package = types.ModuleType("pysbf2")
    package.__path__ = [str(source)]
    sys.modules["pysbf2"] = package
    from pysbf2 import sbftypes_blocks
    from pysbf2.sbftypes_blocks import SBF_BLOCKS
    from pysbf2.sbftypes_core import SBF_MSGIDS

    ids = {value[0]: key for key, value in SBF_MSGIDS.items()}
    # Legacy block retained by the existing RawBits adapter; same header/body
    # layout as L6D, with Source=0 allowing an unspecified L6 service.
    ids["QZSRawL6"] = 4069
    outputs = {
        "sbf_ids_gen.hpp": NOTICE
        + "\n#pragma once\n#include <cstdint>\nnamespace cppgnss { enum class SbfMessageId : uint16_t {\n"
        + ",\n".join(f"{enum_name(name)} = {value}" for name, value in ids.items())
        + "\n}; }\n"
    }
    seen = []
    dispatch = [NOTICE]
    for group, attr in GROUPS.items():
        blocks = copy.deepcopy(getattr(sbftypes_blocks, "SBF_" + attr + "_BLOCKS"))
        if group == "navigation_page":
            blocks["QZSRawL6"] = copy.deepcopy(blocks["QZSRawL6D"])
        if group == "measurement":
            _, fields = blocks["MeasExtra"]["group"]
            tail = {key: fields.pop(key) for key in ("CumLossCont", "CarMPCorr", "Info", "Misc")}
            # Keep padding last regardless of the upstream PAD field spelling.
            pads = {key: fields.pop(key) for key in list(fields) if fields[key] == "SBLength"}
            fields["revision1"] = (("__revision", [1, 2, 3]), {key: tail[key] for key in ("CumLossCont", "CarMPCorr")})
            fields["revision2"] = (("__revision", [2, 3]), {"Info": tail["Info"]})
            fields["revision3"] = (("__revision", 3), {"Misc": tail["Misc"]})
            fields.update(pads)
        seen.extend(blocks)
        hpp, cpp = generate_group(group, blocks, ids)
        outputs[f"sbf_{group}_gen.hpp"] = hpp
        outputs[f"sbf_{group}_gen.cpp"] = cpp
        dispatch.append(f"#include <cppgnss/sbf_{group}_gen.hpp>")
    if [name for name in seen if name != "QZSRawL6"] != list(SBF_BLOCKS):
        raise ValueError("SBF group coverage/order differs from SBF_BLOCKS")
    dispatch.append("namespace cppgnss::SBF {")
    for group in GROUPS:
        dispatch.append(f"const std::vector<Schema> &schemas_{group}();")
    dispatch += ["const std::vector<Schema>& schemas() { static const auto value=[] { std::vector<Schema> all;"]
    for group in GROUPS:
        dispatch.append(f"auto &{group}=schemas_{group}(); all.insert(all.end(),{group}.begin(),{group}.end());")
    dispatch += [
        "return all; }(); return value; }",
        "BlockInfo inspect(uint16_t id,uint8_t revision,std::span<const uint8_t> payload) { switch(id) {",
    ]
    for name in seen:
        dispatch.append(
            f"case {ids[name]}: return detail::info(id,revision,{json.dumps(name)},parse<{name}>(FrameView{{SbfHeader{{id,revision}},0,{{}},payload}}));"
        )
    dispatch += [
        'default: return {id,revision,"",Status::unknown_block,0,{},std::nullopt,std::nullopt}; } }',
        "}",
        "namespace cppgnss::detail { std::string dump_sbf(const FrameView &frame) { switch(frame.id()) {",
    ]
    for name in seen:
        dispatch.append(f"case {ids[name]}: {{ auto result = parse<SBF::{name}>(frame); return dump_parsed(frame, result); }}")
    dispatch += [
        "default: return dump_raw(frame); } }",
        "}",
    ]
    outputs["sbf_dispatch_gen.cpp"] = "\n".join(dispatch) + "\n"
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, content in outputs.items():
        path = output_dir / name
        if not path.exists() or path.read_text() != content:
            path.write_text(content)
    click.echo(f"Generated {len(seen)} typed SBF blocks in {len(GROUPS)} groups.")


if __name__ == "__main__":
    cli()
