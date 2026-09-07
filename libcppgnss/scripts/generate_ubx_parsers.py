#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Kelei Chen

"""
Generate C++ UBX message parsers (structs + parser classes with endianness conversion)
from pyubx2's Python payload definitions.

Usage:
    python scripts/generate_ubx_parsers.py --output-dir BUILD/generated/cppgnss

Output:
    BUILD/generated/cppgnss/ubx_struct_gen.hpp  - Packed struct definitions
    BUILD/generated/cppgnss/ubx_{class}_gen.hpp  - Parser class declarations (one per UBX class)
    BUILD/generated/cppgnss/ubx_{class}_gen.cpp  - Parser implementations
"""

import json
import os
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any

import click

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
OUTPUT_DIR = None

# Path to pyubx2 types modules
PYUBX2_DIR = os.path.join(os.path.dirname(PROJECT_DIR), "contrib", "pyubx2", "src")
sys.path.insert(0, PYUBX2_DIR)

TARGET_CLASSES = ("NAV", "RXM", "MON", "TIM", "ESF", "HNR", "LOG", "SEC", "CFG", "ACK")
SKIP_MESSAGES = {"FOO-BAR"}


@dataclass(frozen=True)
class VariantInfo:
    identity: str
    offset: int | None = None  # None selects by normalized fixed wire length.
    value: int | None = None


# Wire discriminators from pyubx2/ubxvariants.py and payload version comments.
# Payload fields and sizes always come from UBX_PAYLOADS_GET, never this registry.
VARIANTS = {
    "CFG-NMEAvX": VariantInfo("CFG-NMEA"),
    "CFG-NMEAv0": VariantInfo("CFG-NMEA"),
    "CFG-NMEA": VariantInfo("CFG-NMEA"),
    "NAV-AOPSTATUS-L": VariantInfo("NAV-AOPSTATUS"),
    "NAV-AOPSTATUS": VariantInfo("NAV-AOPSTATUS"),
    "NAV-DAHEADINGHP": VariantInfo("NAV-DAHEADING", 0, 1),
    "NAV-DAHEADING": VariantInfo("NAV-DAHEADING", 0, 2),
    "NAV-RELPOSNED-V0": VariantInfo("NAV-RELPOSNED", 0, 0),
    "NAV-RELPOSNED": VariantInfo("NAV-RELPOSNED", 0, 1),
    "RXM-PMP-V0": VariantInfo("RXM-PMP", 0, 0),
    "RXM-PMP-V1": VariantInfo("RXM-PMP", 0, 1),
    "RXM-RLM-S": VariantInfo("RXM-RLM", 1, 1),
    "RXM-RLM-L": VariantInfo("RXM-RLM", 1, 2),
    "SEC-SIG-V1": VariantInfo("SEC-SIG", 0, 1),
    "SEC-SIG-V2": VariantInfo("SEC-SIG", 0, 2),
    "SEC-UNIQID": VariantInfo("SEC-UNIQID", 0, 1),
    "SEC-UNIQID-V2": VariantInfo("SEC-UNIQID", 0, 2),
}


def variant_condition(name, fields):
    variant = VARIANTS.get(name)
    if variant is None:
        return "true"
    if variant.offset is None:
        size = compute_struct_size(fields)
        if size == 0:
            raise ValueError(f"{name}: length-selected variant must be fixed-size")
        return f"frame.length == {size}"
    return f"frame.payload.size() > {variant.offset} && " f"frame.payload[{variant.offset}] == {variant.value}"


@dataclass
class BitFieldInfo:
    name: str
    bits: int


@dataclass
class FieldInfo:
    name: str
    pytype: str | None = None
    ctype: str = "uint8_t"
    arr_size: int = 0
    size: int = 0
    is_scaled: bool = False
    is_bitfield: bool = False
    is_repeating: bool = False
    is_reserved: bool = False
    scale_factor: Any = None
    bit_fields: list[BitFieldInfo] = field(default_factory=list)
    repeat_count: int | str | None = None
    nested_fields: list["FieldInfo"] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Type mapping: Python type string -> C++ type
# ---------------------------------------------------------------------------

X_HEX_WIDTH = {
    "X001": 2,
    "X002": 4,
    "X004": 8,
    "X008": 16,
}

# ---------------------------------------------------------------------------
# Type info
# ---------------------------------------------------------------------------


def parse_pytype(pytype: str) -> dict:
    """Parse a pytype string like 'U004', 'C030', 'R008' into base type info."""
    match = re.fullmatch(r"([UIRXCAEL])(\d{3})", pytype)
    if not match:
        raise ValueError(f"Unsupported wire type: {pytype!r}")
    prefix, width = match.groups()
    size = int(width)
    if size == 0 or (prefix == "R" and size not in (4, 8)):
        raise ValueError(f"Unsupported wire type: {pytype!r}")
    if prefix == "R":
        return {
            "base_type": "float" if size == 4 else "double",
            "size": size,
            "is_array": False,
        }
    if prefix in "UIXEL" and size in (1, 2, 4, 8):
        base = ("int" if prefix == "I" else "uint") + str(size * 8) + "_t"
        return {"base_type": base, "size": size, "is_array": False}
    return {"base_type": "uint8_t", "size": size, "is_array": True}


def cpp_field_decl(ctype: str, name: str, arr_size: int = 0) -> str:
    """Emit `type name;` or `type name[size];`"""
    if arr_size > 0:
        return f"{ctype} {name}[{arr_size}];"
    return f"{ctype} {name};"


def find_bitfield_subfield(fields: list[FieldInfo], subfield_name):
    """Find a bitfield containing a named sub-field and return its C++ extraction expression.
    Returns None if not found in any bitfield."""
    for f in fields:
        if f.is_bitfield and f.bit_fields:
            offset = 0
            for bf in f.bit_fields:
                if bf.name == subfield_name:
                    bits = bf.bits
                    mask = (1 << bits) - 1
                    return f"((this->{f.name} >> {offset}) & UINT64_C(0x{mask:x}))"
                offset += bf.bits
    return None


# ---------------------------------------------------------------------------
# Payload definition parser
# ---------------------------------------------------------------------------


def parse_payload_def(payload_def: dict, path: str = "payload") -> list[FieldInfo]:
    """Normalize upstream schema once, rejecting unsupported shapes with a path.

    For a group, size is the wire size of ONE element, including nested fixed
    repetition. A dynamic group's total size is never represented as fixed.
    """
    fields = []
    counts = set()
    for name, definition in payload_def.items():
        location = f"{path}.{name}"
        info = FieldInfo(name=name, is_reserved=bool(re.fullmatch(r"reserved\d*", name)))
        raw_type = None
        if definition == "CH":
            definition = (None, {"byte": "U001"})
        if isinstance(definition, str):
            raw_type = definition
        elif isinstance(definition, (list, tuple)) and len(definition) == 2:
            kind, value = definition
            if isinstance(value, (int, float)):
                raw_type = kind
                info.is_scaled = True
                info.scale_factor = value
            elif isinstance(definition, tuple) and isinstance(value, dict):
                if isinstance(kind, str) and kind.startswith("X"):
                    raw_type = kind
                    info.is_bitfield = True
                    for bit_name, bit_type in value.items():
                        match = re.fullmatch(r"[UXI](\d{3})", str(bit_type))
                        if not match or int(match[1]) == 0:
                            raise ValueError(f"{location}.{bit_name}: unsupported bit width {bit_type!r}")
                        info.bit_fields.append(BitFieldInfo(bit_name, int(match[1])))
                        counts.add(bit_name)
                else:
                    info.is_repeating = True
                    info.repeat_count = None if kind == "None" else kind
                    if isinstance(info.repeat_count, int):
                        if info.repeat_count <= 0:
                            raise ValueError(f"{location}: repeat count must be positive")
                    elif info.repeat_count is not None and info.repeat_count not in counts:
                        raise ValueError(f"{location}: unknown or forward repeat count {kind!r}")
                    info.nested_fields = parse_payload_def(value, location)
                    if not is_fixed_size(info.nested_fields):
                        raise ValueError(f"{location}: nested dynamic groups are not supported")
                    info.size = compute_struct_size(info.nested_fields)
                    if info.size == 0:
                        raise ValueError(f"{location}: empty repeating group")
            else:
                raise TypeError(f"{location}: unsupported field definition {definition!r}")
        else:
            raise TypeError(f"{location}: unsupported field definition {definition!r}")
        if raw_type is None and not info.is_repeating:
            raise ValueError(f"{location}: missing wire type")
        if raw_type is not None:
            try:
                wire_type = parse_pytype(raw_type)
            except (ValueError, TypeError) as error:
                raise ValueError(f"{location}: {error}") from error
            info.pytype = raw_type
            info.ctype = wire_type["base_type"]
            info.size = wire_type["size"]
            info.arr_size = info.size if wire_type["is_array"] else 0
            if info.is_bitfield and (info.arr_size or sum(b.bits for b in info.bit_fields) > info.size * 8):
                raise ValueError(f"{location}: bitfields exceed container width")
            if not info.arr_size and not raw_type.startswith("R"):
                counts.add(name)
        fields.append(info)
    for index, info in enumerate(fields):
        if info.is_repeating and info.repeat_count is None and index != len(fields) - 1:
            raise ValueError(f"{path}.{info.name}: remaining-payload group must be last")
    return fields


def is_fixed_size(fields):
    return all(not f.is_repeating or (isinstance(f.repeat_count, int) and is_fixed_size(f.nested_fields)) for f in fields)


def compute_struct_size(fields):
    if not is_fixed_size(fields):
        return 0
    return sum((compute_struct_size(f.nested_fields) * f.repeat_count if f.is_repeating else f.size) for f in fields)


# ---------------------------------------------------------------------------
# Name helpers
# ---------------------------------------------------------------------------


def struct_name(msg_name):
    return "_ubx_" + msg_name.lower().replace("-", "_")


def parser_class_name(msg_name):
    return "ubx_" + msg_name.lower().replace("-", "_")


def msg_id_const(msg_name):
    return "UBX_" + "_".join(p.upper() for p in msg_name.split("-"))


def class_id_const(cls: str):
    return "UBX_CLASS_" + cls.upper()


def is_generated_msg_name(msg_name):
    return msg_name not in SKIP_MESSAGES and not msg_name.startswith("UBX-")


def should_generate_parser(msg_name, ubx_payloads):
    return msg_name in ubx_payloads and is_generated_msg_name(msg_name)


def should_generate_struct(msg_name, ubx_payloads):
    return should_generate_parser(msg_name, ubx_payloads)


def should_generate_dump_case(msg_name, ubx_payloads):
    return msg_name in ubx_payloads and is_generated_msg_name(msg_name)


def write_output(path, content):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == content:
                print(f"Unchanged: {path}")
                return

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"Wrote: {path}")


# ---------------------------------------------------------------------------
# Struct generation
# ---------------------------------------------------------------------------


def gen_struct_inner(fields, struct_tag, indent=1, packed=True):
    """Declare scalars and arbitrarily nested fixed groups from normalized fields."""
    lines = []
    tab = "\t" * indent
    for f in fields:
        if f.is_repeating:
            inner_name = f"{struct_tag}_{f.name}_t"
            lines.extend([f"{tab}struct {inner_name}", f"{tab}{{"])
            lines.extend(gen_struct_inner(f.nested_fields, inner_name, indent + 1, packed))
            attribute = " __attribute__((packed))" if packed else ""
            if isinstance(f.repeat_count, int):
                lines.append(f"{tab}}}{attribute} {f.name}[{f.repeat_count}];")
            else:
                lines.extend([f"{tab}}};", f"{tab}vector<{inner_name}> {f.name};"])
        else:
            lines.append(f"{tab}{cpp_field_decl(f.ctype, f.name, f.arr_size)}")
    return lines


def generate_struct(msg_name, fields):
    struct = struct_name(msg_name)
    sz = compute_struct_size(fields)
    lines = []
    lines.append(f"// {msg_name}")
    lines.append(f"struct {struct}")
    lines.append("{")
    lines.extend(gen_struct_inner(fields, struct, 1))
    lines.append("} __attribute__((packed));")
    lines.append("")
    if sz > 0:
        lines.append(f'static_assert(sizeof({struct}) == {sz}, "{struct} size mismatch");')
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Parser class header
# ---------------------------------------------------------------------------


def generate_parser_header(msg_name, fields, ubx_class):
    cls = parser_class_name(msg_name)
    struct = struct_name(msg_name)
    fixed = is_fixed_size(fields) and compute_struct_size(fields) > 0

    lines = []
    lines.append(f"class {cls} : public ubx_any_msg")
    lines.append("{")
    lines.append("public:")
    if fixed:
        lines.append(f"\tstruct {struct} data;")
    else:
        lines.extend(gen_struct_inner(fields, struct, packed=False))
    lines.append("")

    lines.append(f"\t{cls}();")
    lines.append(f"\t{cls}(const ubx_frame &frame);")
    lines.append(f"\tbool parse(const ubx_frame &frame);")
    lines.append(f"\tvoid clear();")
    lines.append(f"\tvoid dump(FILE *fp) const;")
    lines.append("")
    lines.append("};")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Parser implementation
# ---------------------------------------------------------------------------


def gen_read_field(f, prefix, tab):
    """Generate bounds-checked code to read one scalar/array field."""
    lines = [
        f"{tab}if(off > frame.length || size_t({f.size}) > frame.length - off)",
        f"{tab}{{",
        f'{tab}\treport_parse_error(std::format("field {f.name} at offset {{}} exceeds payload length {{}}", off, frame.length));',
        f"{tab}\treturn false;",
        f"{tab}}}",
    ]

    if not f.arr_size:
        lines.append(f"{tab}{prefix}{f.name} = read_le<{f.ctype}>(frame.payload, off);")
    else:
        sz = f.size
        lines.append(f"{tab}memcpy(&{prefix}{f.name}, frame.payload.data() + off, {sz});")

    lines.append(f"{tab}off += {f.size};")
    return lines


def generate_parser_impl(msg_name, fields, ubx_class):
    cls = parser_class_name(msg_name)
    struct = struct_name(msg_name)
    fixed = is_fixed_size(fields) and compute_struct_size(fields) > 0
    total_sz = compute_struct_size(fields)

    lines = []

    # Constructor
    lines.append(f"{cls}::{cls}() {{ clear(); }}")
    lines.append("")
    lines.append(f"{cls}::{cls}(const ubx_frame &frame) {{ parse(frame); }}")
    lines.append("")

    # clear()
    lines.append(f"void {cls}::clear()")
    lines.append("{")
    if fixed:
        lines.append("\tmemset(&this->data, 0, sizeof(this->data));")
    else:
        for f in fields:
            if f.is_repeating and not isinstance(f.repeat_count, int):
                continue  # handled below
            if f.is_repeating:
                lines.append(f"\tmemset(&this->{f.name}, 0, sizeof(this->{f.name}));")
                continue
            if f.pytype is None:
                continue
            if f.arr_size:
                lines.append(f"\tmemset(&this->{f.name}, 0, sizeof(this->{f.name}));")
            else:
                lines.append(f"\tthis->{f.name} = 0;")
    for f in fields:
        if f.is_repeating and not isinstance(f.repeat_count, int):
            lines.append(f"\tthis->{f.name}.clear();")
    lines.append("\tubx_any_msg::clear();")
    lines.append("}")
    lines.append("")

    # parse()
    lines.append(f"bool {cls}::parse(const ubx_frame &frame)")
    lines.append("{")
    lines.append("\tthis->clear();")
    lines.append("\tif(!frame.valid || frame.payload.size() != frame.length) return false;")
    lines.append("")
    cc = class_id_const(ubx_class)
    mc = msg_id_const(msg_name)
    lines.append(f"\tif(frame.class_id != {cc} || frame.msg_id != {mc}) return false;")
    lines.append("")

    if msg_name in VARIANTS:
        lines.append(f"\tif(!({variant_condition(msg_name, fields)})) return false;")

    if fixed and total_sz > 0:
        lines.append(f"\tif(frame.length != sizeof(this->data))")
        lines.append("\t{")
        lines.append(
            f'\t\treport_parse_error(std::format("{cls}: length {{}} != expected {{}}", frame.length, sizeof(this->data)));'
        )
        lines.append("\t\treturn false;")
        lines.append("\t}")
        lines.append("")
        lines.append("\tsize_t off = 0;")
        lines.extend(gen_read_fields(fields, "this->data.", "\t"))
    else:
        # Field-by-field parsing for variable-size messages
        lines.append("\tsize_t off = 0;")
        lines.append("")
        for f in fields:
            if f.is_repeating:
                rc = f.repeat_count
                elem_sz = f.size
                nest_type = f"{struct}_{f.name}_t"

                if rc == "None" or rc is None:
                    lines.append(f"\t// repeating group: {f.name}")
                    lines.append(f"\twhile(off + {elem_sz} <= frame.length)")
                    lines.append("\t{")
                    lines.append(f"\t\t{nest_type} item{{}};")
                    lines.extend(gen_read_fields(f.nested_fields, "item.", "\t\t"))
                    lines.append(f"\t\tthis->{f.name}.push_back(item);")
                    lines.append("\t}")
                elif isinstance(rc, str):
                    # Repeat count may be a top-level field or a bitfield sub-field
                    count_expr = find_bitfield_subfield(fields, rc)
                    if count_expr is None:
                        count_expr = f"this->{rc}"
                    count_name = f"count_{f.name}"
                    lines.append(f"\tconst size_t {count_name} = static_cast<size_t>({count_expr});")
                    lines.append(f"\tif({count_name} > (frame.length - off) / {elem_sz})")
                    lines.append("\t{")
                    lines.append(f'\t\treport_parse_error("{msg_name}.{f.name}: repeat count exceeds remaining payload");')
                    lines.append("\t\treturn false;")
                    lines.append("\t}")
                    lines.append(f"\tthis->{f.name}.reserve({count_name});")
                    lines.append(f"\tfor(size_t i = 0; i < {count_name}; i++)")
                    lines.append("\t{")
                    lines.append(f"\t\t{nest_type} item{{}};")
                    lines.extend(gen_read_fields(f.nested_fields, "item.", "\t\t"))
                    lines.append(f"\t\tthis->{f.name}.push_back(item);")
                    lines.append("\t}")
                elif isinstance(rc, int):
                    lines.append(f"\t// fixed repeating group: {f.name} x {rc}")
                    lines.append(f"\tfor(int i = 0; i < {rc}; i++)")
                    lines.append("\t{")
                    lines.extend(gen_read_fields(f.nested_fields, f"this->{f.name}[i].", "\t\t"))
                    lines.append("\t}")
            elif f.is_bitfield:
                lines.extend(gen_read_field(f, "this->", "\t"))
            else:
                lines.extend(gen_read_field(f, "this->", "\t"))

    lines.append("")
    if not fixed:
        lines.append("\tif(off != frame.length) return false;")
        lines.append("")
    lines.append("\tthis->valid = true;")
    lines.append("\treturn true;")
    lines.append("}")
    lines.append("")

    # dump()
    lines.append(f"void {cls}::dump(FILE *fp) const")
    lines.append("{")
    lines.append(f'\tstd::string output = "({msg_name}";')
    if fixed:
        for f in fields:
            if f.is_reserved:
                continue
            if f.is_repeating and isinstance(f.repeat_count, int):
                lines.append(f'\toutput += std::format(", {f.name}={{}} items x {{}} bytes", {f.repeat_count}, {f.size});')
            elif not f.is_repeating and f.pytype:
                pt = f.pytype[:4]
                if f.arr_size:
                    lines.append(f'\toutput += std::format(", {f.name}={{}} bytes", sizeof(data.{f.name}));')
                elif pt.startswith("X"):
                    width = X_HEX_WIDTH.get(pt, 2)
                    lines.append(f'\toutput += std::format(", {f.name}=0x{{:0{width}x}}", +data.{f.name});')
                else:
                    lines.append(f'\toutput += std::format(", {f.name}={{}}", +data.{f.name});')
    else:
        for f in fields:
            if f.is_reserved:
                continue
            if f.is_repeating and not isinstance(f.repeat_count, int):
                lines.append(f'\toutput += std::format(", {f.name}={{}} items", this->{f.name}.size());')
            elif f.is_repeating and isinstance(f.repeat_count, int):
                continue
            elif not f.is_repeating and f.pytype:
                pt = f.pytype[:4]
                if f.arr_size:
                    lines.append(f'\toutput += std::format(", {f.name}={{}} bytes", sizeof(this->{f.name}));')
                elif pt.startswith("X"):
                    width = X_HEX_WIDTH.get(pt, 2)
                    lines.append(f'\toutput += std::format(", {f.name}=0x{{:0{width}x}}", +this->{f.name});')
                else:
                    lines.append(f'\toutput += std::format(", {f.name}={{}}", +this->{f.name});')
    lines.append('\toutput += ")\\n";')
    lines.append("\tfputs(output.c_str(), fp);")
    lines.append("}")
    lines.append("")

    return "\n".join(lines)


def gen_read_fields(fields, prefix, tab, depth=0):
    """Generate code to read fields from payload at offset `off`."""
    lines = []
    for f in fields:
        if f.is_repeating and isinstance(f.repeat_count, int):
            lines.append(f"{tab}// nested fixed rg {f.name} x {f.repeat_count}")
            lines.append(f"{tab}for(size_t j{depth} = 0; j{depth} < {f.repeat_count}; j{depth}++)")
            lines.append(f"{tab}{{")
            lines.extend(
                gen_read_fields(
                    f.nested_fields,
                    f"{prefix}{f.name}[j{depth}].",
                    tab + "\t",
                    depth + 1,
                )
            )
            lines.append(f"{tab}}}")
        else:
            lines.extend(gen_read_field(f, prefix, tab))
    return lines


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def get_interesting_classes():
    from pyubx2.ubxtypes_core import UBX_CLASSES, UBX_MSGIDS
    from pyubx2.ubxtypes_get import UBX_PAYLOADS_GET

    msg_to_ids = {}
    for key, name in UBX_MSGIDS.items():
        if len(key) == 2:
            msg_to_ids[name] = (key[0], key[1])

    for name, variant in VARIANTS.items():
        if name not in UBX_PAYLOADS_GET or variant.identity not in msg_to_ids:
            raise ValueError(f"Missing upstream variant definition: {name}")
        msg_to_ids[name] = msg_to_ids[variant.identity]

    missing = [name for name in UBX_PAYLOADS_GET if name.split("-")[0] in TARGET_CLASSES and name not in msg_to_ids]
    if missing:
        raise ValueError(f"Payloads require explicit wire identity/variant rules: {missing}")

    classes = OrderedDict()
    for msg_name in sorted(UBX_PAYLOADS_GET.keys()):
        if msg_name in msg_to_ids:
            cls_id, msg_id = msg_to_ids[msg_name]
            cls_name = UBX_CLASSES.get(bytes([cls_id]), f"C_{cls_id:02x}")
            base = cls_name.split("-")[0]
            if base not in classes:
                classes[base] = []
            classes[base].append((msg_name, cls_id, msg_id))
    return classes


HEADER = """/*
 * Auto-generated UBX message parser definitions.
 * Generated from pyubx2 payload definitions by scripts/generate_ubx_parsers.py.
 *
 * UBX payload definitions derived from pyubx2:
 *   Copyright (c) 2020, semuadin (Steve Smith)
 *   SPDX-License-Identifier: BSD-3-Clause
 *   https://github.com/semuconsulting/pyubx2
 *
 * This file is part of libcppgnss in NeoGNSS Observatory.
 * The generator originated in rpi-gnss-server.
 * Copyright (c) 2026, Kelei Chen
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

"""


def write_struct_file(class_msgs, ubx_payloads):
    lines = [HEADER]
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("#include <stdint.h>")
    lines.append("#include <cppgnss/ubx_ids_gen.hpp>")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace UBX")
    lines.append("{")
    lines.append("")
    for msgs in class_msgs.values():
        for msg_name, _, _ in msgs:
            if not should_generate_struct(msg_name, ubx_payloads):
                continue
            fields = ubx_payloads[msg_name]
            if is_fixed_size(fields) and compute_struct_size(fields) > 0:
                lines.append(generate_struct(msg_name, fields))
    lines.append("")
    lines.append("} // namespace UBX")
    lines.append("")
    return "\n".join(lines)


def write_parser_file(class_name, msgs, ubx_payloads):
    lines = [HEADER]
    lines.append(f"#ifndef UBX_{class_name.upper()}_GEN_HPP")
    lines.append(f"#define UBX_{class_name.upper()}_GEN_HPP")
    lines.append("")
    lines.append("#include <cppgnss/ubx_def.hpp>")
    lines.append("#include <cppgnss/ubx_struct_gen.hpp>")
    lines.append("#include <vector>")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace UBX")
    lines.append("{")
    lines.append("using std::vector;")
    lines.append("")
    for msg_name, _, _ in msgs:
        if not should_generate_parser(msg_name, ubx_payloads):
            continue
        fields = ubx_payloads[msg_name]
        lines.append(generate_parser_header(msg_name, fields, class_name))
        lines.append("")
    lines.append("} // namespace UBX")
    lines.append("")
    lines.append(f"#endif // UBX_{class_name.upper()}_GEN_HPP")
    lines.append("")
    return "\n".join(lines)


def write_parser_impl_file(class_name, msgs, ubx_payloads):
    fn = f"ubx_{class_name.lower()}_gen"
    lines = [HEADER]
    lines.append(f"#include <cppgnss/{fn}.hpp>")
    lines.append("#include <cppgnss/ubx_ids_gen.hpp>")
    lines.append("#include <string>")
    lines.append("#include <format>")
    lines.append("#include <cstring>")
    lines.append("")
    lines.append("namespace UBX")
    lines.append("{")
    lines.append("")
    for msg_name, _, _ in msgs:
        if not should_generate_parser(msg_name, ubx_payloads):
            continue
        fields = ubx_payloads[msg_name]
        lines.append(generate_parser_impl(msg_name, fields, class_name))
        lines.append("")
    lines.append("} // namespace UBX")
    lines.append("")
    return "\n".join(lines)


def write_dump_gen_header():
    lines = [HEADER]
    lines.append("#ifndef UBX_DUMP_GEN_HPP")
    lines.append("#define UBX_DUMP_GEN_HPP")
    lines.append("")
    lines.append("#include <cppgnss/ubx_def.hpp>")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace UBX")
    lines.append("{")
    lines.append("void ubx_dump_any(const ubx_frame &frame, FILE *fp);")
    lines.append("} // namespace UBX")
    lines.append("")
    lines.append("#endif // UBX_DUMP_GEN_HPP")
    return "\n".join(lines)


def write_dump_gen_impl(class_msgs, target_classes, ubx_payloads):
    lines = [HEADER]
    lines.append("#include <cppgnss/ubx_dump_gen.hpp>")
    lines.append("#include <cppgnss/ubx_ids_gen.hpp>")
    lines.append("")
    for cls_name in target_classes:
        if cls_name in class_msgs:
            lines.append(f"#include <cppgnss/ubx_{cls_name.lower()}_gen.hpp>")
    lines.append("")
    lines.append("namespace UBX")
    lines.append("{")
    lines.append("")
    lines.append("void ubx_dump_any(const ubx_frame &frame, FILE *fp)")
    lines.append("{")
    lines.append("    if (!frame.valid)")
    lines.append("        return;")
    lines.append("")
    lines.append("    switch (frame.class_id)")
    lines.append("    {")
    for cls_name in target_classes:
        if cls_name not in class_msgs:
            continue
        msgs = class_msgs[cls_name]
        valid = [(n, c, i) for n, c, i in msgs if should_generate_dump_case(n, ubx_payloads)]
        if not valid:
            continue
        cc = class_id_const(cls_name)
        lines.append(f"    case {cc}:")
        lines.append(f"        switch (frame.msg_id)")
        lines.append(f"        {{")
        by_id = {}
        for name, _, msg_id in valid:
            by_id.setdefault(msg_id, []).append(name)
        for msg_id, names in by_id.items():
            lines.append(f"        case 0x{msg_id:02x}:")
            for name in names:
                condition = variant_condition(name, ubx_payloads[name])
                pc = parser_class_name(name)
                lines.append(f"            if({condition}) {{ {pc} p(frame); if(p.valid) {{ p.dump(fp); return; }} }}")
            lines.append("            break;")
        lines.append(f"        default:")
        lines.append(f"            break;")
        lines.append(f"        }}")
        lines.append(f"        break;")
    lines.append("    default:")
    lines.append("        break;")
    lines.append("    }")
    lines.append("    ubx_any_msg(frame).dump(fp);")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace UBX")
    return "\n".join(lines)


def write_ids_file(class_msgs, target_classes):
    """Generate ubx_ids_gen.hpp with all UBX class/message ID constants."""
    lines = [HEADER]
    lines.append("#ifndef UBX_IDS_GEN_HPP")
    lines.append("#define UBX_IDS_GEN_HPP")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace UBX")
    lines.append("{")
    lines.append("")
    for cls_name in target_classes:
        if cls_name not in class_msgs:
            continue
        msgs = class_msgs[cls_name]
        cls_id = msgs[0][1]
        cc = class_id_const(cls_name)
        lines.append(f"// class {cls_name}")
        lines.append(f"constexpr uint8_t {cc} = 0x{cls_id:02x};")
        lines.append("")
        ids = {name: msg_id for name, _, msg_id in msgs if is_generated_msg_name(name)}
        ids.update({VARIANTS[name].identity: msg_id for name, msg_id in list(ids.items()) if name in VARIANTS})
        for name, msg_id in sorted(ids.items()):
            lines.append(f"constexpr uint8_t {msg_id_const(name)} = 0x{msg_id:02x};")
        lines.append("")
    lines.append("} // namespace UBX")
    lines.append("")
    lines.append("#endif // UBX_IDS_GEN_HPP")
    lines.append("")
    return "\n".join(lines)


def get_name_tables():
    """Keep name coverage independent of parser support and GET/SET/POLL mode."""
    from pyubx2.ubxtypes_core import UBX_CLASSES, UBX_MSGIDS

    classes = {key[0]: name for key, name in UBX_CLASSES.items() if name != "FOO"}
    messages = {}
    subtypes = {}
    families = {}
    for key, name in sorted(UBX_MSGIDS.items()):
        if name in SKIP_MESSAGES:
            continue
        if len(key) == 2:
            messages[int.from_bytes(key, "big")] = name
        elif len(key) == 3 and classes.get(key[0]) == "MGA":
            # MGA keys append the type byte at payload offset 0.
            subtypes[int.from_bytes(key, "big")] = name
            families.setdefault(int.from_bytes(key[:2], "big"), set()).add(name.split("-")[1])
        else:
            raise ValueError(f"Unsupported UBX name key {key.hex()}: {name}")

    for key, family_names in families.items():
        # Without a payload only the family is known. 0x1360 is shared by
        # MGA-ACK and MGA-NAK, so keep both rather than guessing the subtype.
        messages.setdefault(key, "MGA-" + "/".join(sorted(family_names)))
    return classes, messages, subtypes


def write_names_file():
    classes, messages, subtypes = get_name_tables()
    lines = [
        HEADER,
        "#include <cppgnss/ubx_names.hpp>",
        "",
        "namespace UBX",
        "{",
        "string ubx_msg_name(uint8_t class_id, uint8_t msg_id)",
        "{",
        "\treturn ubx_msg_name(class_id, msg_id, {});",
        "}",
        "",
        "string ubx_msg_name(uint8_t class_id, uint8_t msg_id, std::span<const uint8_t> payload)",
        "{",
        "\tuint32_t id = (uint32_t(class_id) << 8) | msg_id;",
        "\tif(!payload.empty())",
        "\t{",
        "\t\tswitch((id << 8) | payload[0])",
        "\t\t{",
    ]
    for key, name in sorted(subtypes.items()):
        lines.append(f"\t\tcase 0x{key:06x}: return {json.dumps(name)};")
    lines.extend(["\t\t}", "\t}", "\tswitch(id)", "\t{"])
    for key, name in sorted(messages.items()):
        lines.append(f"\tcase 0x{key:04x}: return {json.dumps(name)};")
    lines.extend(
        [
            "\t}",
            "\tchar buf[32];",
            '\tsnprintf(buf, sizeof(buf), "%#02x", msg_id);',
            "\tswitch(class_id)",
            "\t{",
        ]
    )
    for key, name in sorted(classes.items()):
        lines.append(f'\tcase 0x{key:02x}: return string({json.dumps(name)}) + "-" + buf;')
    lines.extend(
        [
            "\t}",
            '\tsnprintf(buf, sizeof(buf), "%#02x-%#02x", class_id, msg_id);',
            "\treturn string(buf);",
            "}",
            "",
            "} // namespace UBX",
            "",
        ]
    )
    return "\n".join(lines)


def generate(output_dir):
    global OUTPUT_DIR
    OUTPUT_DIR = os.fspath(output_dir)
    from pyubx2.ubxtypes_get import UBX_PAYLOADS_GET

    class_msgs = get_interesting_classes()
    print(f"Found {len(class_msgs)} classes:")
    for cls_name, msgs in sorted(class_msgs.items()):
        print(f"  {cls_name}: {len(msgs)} messages")

    # Validate the complete selected schema before writing any generated files.
    schemas = {
        name: parse_payload_def(UBX_PAYLOADS_GET[name], name)
        for messages in class_msgs.values()
        for name, _, _ in messages
        if is_generated_msg_name(name)
    }
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    write_output(os.path.join(OUTPUT_DIR, "ubx_names_gen.cpp"), write_names_file())

    # Generate ID constants
    ids_content = write_ids_file(class_msgs, TARGET_CLASSES)
    p = os.path.join(OUTPUT_DIR, "ubx_ids_gen.hpp")
    write_output(p, ids_content)

    # Generate structs
    struct_content = write_struct_file(class_msgs, schemas)
    p = os.path.join(OUTPUT_DIR, "ubx_struct_gen.hpp")
    write_output(p, struct_content)

    # Generate parser files for interesting classes
    for cls_name in TARGET_CLASSES:
        if cls_name not in class_msgs:
            continue
        msgs = class_msgs[cls_name]
        # Messages that have hand-written parsers -> skip duplication
        valid = [(n, c, i) for n, c, i in msgs if should_generate_parser(n, UBX_PAYLOADS_GET)]
        if not valid:
            continue

        hpp = write_parser_file(cls_name, valid, schemas)
        hp = os.path.join(OUTPUT_DIR, f"ubx_{cls_name.lower()}_gen.hpp")
        write_output(hp, hpp)

        cpp = write_parser_impl_file(cls_name, valid, schemas)
        cp = os.path.join(OUTPUT_DIR, f"ubx_{cls_name.lower()}_gen.cpp")
        write_output(cp, cpp)

    # Generate universal dump function
    dump_hpp = write_dump_gen_header()
    dp = os.path.join(OUTPUT_DIR, "ubx_dump_gen.hpp")
    write_output(dp, dump_hpp)

    dump_cpp = write_dump_gen_impl(class_msgs, TARGET_CLASSES, schemas)
    dp = os.path.join(OUTPUT_DIR, "ubx_dump_gen.cpp")
    write_output(dp, dump_cpp)

    print("\nDone.")


@click.command()
@click.option(
    "--output-dir",
    required=True,
    type=click.Path(file_okay=False, path_type=str),
    help="Generated cppgnss header and source directory.",
)
def cli(output_dir):
    """Generate C++ UBX decoders from the repository's pinned pyubx2 schema."""
    generate(output_dir)


if __name__ == "__main__":
    cli()
