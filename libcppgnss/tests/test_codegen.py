#!/usr/bin/env python3
"""Regression tests for the pyubx2 schema consumed by the C++ generator."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from generate_ubx_parsers import (
    VARIANTS,
    compute_struct_size,
    generate_parser_header,
    generate_parser_impl,
    generate_struct,
    get_name_tables,
    is_fixed_size,
    parse_payload_def,
)
from pyubx2.ubxtypes_core import GET, UBX_CLASSES, UBX_MSGIDS
from pyubx2.ubxtypes_get import UBX_PAYLOADS_GET
from pyubx2.ubxvariants import VARIANTS as UPSTREAM_VARIANTS


class PayloadSchemaTests(unittest.TestCase):
    def test_scaled_tuple_and_legacy_list(self):
        for definition in (("I004", 1e-7), ["I004", 1e-7]):
            with self.subTest(definition=definition):
                (value,) = parse_payload_def({"lon": definition})
                self.assertEqual(value.ctype, "int32_t")
                self.assertEqual(value.size, 4)
                self.assertTrue(value.is_scaled)
                self.assertEqual(value.scale_factor, 1e-7)
                self.assertFalse(value.is_repeating)

    def test_current_nav_pvt(self):
        fields = {f.name: f for f in parse_payload_def(UBX_PAYLOADS_GET["NAV-PVT"])}
        self.assertEqual(sum(f.size for f in fields.values()), 92)
        self.assertTrue(fields["valid_bit"].is_bitfield)
        self.assertTrue(fields["flags_bit"].is_bitfield)
        self.assertTrue(fields["lon"].is_scaled)
        self.assertEqual(fields["lon"].ctype, "int32_t")

    def test_scaled_field_inside_repeating_group(self):
        fields = {f.name: f for f in parse_payload_def(UBX_PAYLOADS_GET["MON-PT2"])}
        group = fields["svsig_grp"]
        self.assertTrue(group.is_repeating)
        self.assertEqual(group.repeat_count, "numSvSigDesc")
        nested = {f.name: f for f in group.nested_fields}
        self.assertEqual(nested["cnoMin"].ctype, "uint16_t")
        self.assertEqual(nested["cnoMin"].scale_factor, 1 / 256)
        self.assertTrue(nested["signalInfo_bit"].is_bitfield)

    def test_invalid_tuple_reports_field(self):
        with self.assertRaisesRegex(TypeError, "badField"):
            parse_payload_def({"badField": ("U004", "invalid scale")})

    def test_unknown_types_and_invalid_groups_fail_with_path(self):
        for definition in (
            "mystery",
            "U004junk",
            "R002",
            "U000",
            (0, {"x": "U001"}),
            ("missingCount", {"x": "U001"}),
            (None, {}),
            ("X001", {"tooWide": "U009"}),
        ):
            with self.subTest(definition=definition), self.assertRaisesRegex(ValueError, "TEST.bad"):
                parse_payload_def({"bad": definition}, "TEST")
        with self.assertRaisesRegex(ValueError, "nested dynamic"):
            parse_payload_def({"outer": (2, {"inner": (None, {"x": "U001"})})})
        with self.assertRaisesRegex(ValueError, "must be last"):
            parse_payload_def({"rest": (None, {"x": "U001"}), "tail": "U001"})

    def test_recursive_wire_size(self):
        fields = parse_payload_def({"outer": (2, {"tag": "U001", "inner": (3, {"x": "U002"})})})
        self.assertTrue(is_fixed_size(fields))
        self.assertEqual(fields[0].size, 7)
        self.assertEqual(compute_struct_size(fields), 14)
        fields = parse_payload_def(UBX_PAYLOADS_GET["MON-COMMS"])
        self.assertFalse(is_fixed_size(fields))
        self.assertEqual(next(f for f in fields if f.name == "ports_grp").size, 40)

    def test_variable_text_is_not_a_one_byte_struct(self):
        fields = parse_payload_def(UBX_PAYLOADS_GET["INF-ERROR"])
        self.assertFalse(is_fixed_size(fields))
        self.assertIsNone(fields[0].repeat_count)
        self.assertEqual(fields[0].size, 1)

    def test_generated_nested_struct_and_decoder(self):
        fields = parse_payload_def(
            {
                "header": "U001",
                "records": (
                    2,
                    {"tag": "U001", "points": (3, {"reading": "I002", "last": "U001"})},
                ),
                "tail": "U002",
            }
        )
        code = "#include <cppgnss/ubx_def.hpp>\n#include <cassert>\n#include <cstring>\n#include <format>\n"
        code += "namespace UBX { constexpr uint8_t UBX_SCHEMA_TEST = 0xfe;\n"
        code += generate_struct("SCHEMA-TEST", fields)
        code += generate_parser_header("SCHEMA-TEST", fields, "ACK")
        code += generate_parser_impl("SCHEMA-TEST", fields, "ACK")
        code += """
}
int main() {
    using namespace UBX;
    static_assert(sizeof(_ubx_schema_test) == 23);
    ubx_frame frame;
    frame.valid = true;
    frame.class_id = UBX_CLASS_ACK;
    frame.msg_id = UBX_SCHEMA_TEST;
    frame.length = 23;
    frame.payload.resize(23);
    // Second record, third point: offset 1 + 10 + 1 + 2*3 = 18.
    frame.payload[18] = 0xfe;
    frame.payload[19] = 0xff;
    frame.payload[20] = 0x42;
    frame.payload[21] = 0x34;
    frame.payload[22] = 0x12;
    ubx_schema_test parsed(frame);
    assert(parsed.valid);
    assert(parsed.data.records[1].points[2].reading == -2);
    assert(parsed.data.records[1].points[2].last == 0x42);
    assert(parsed.data.tail == 0x1234);
}
"""
        directory = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "nested.cpp"
            binary = Path(temporary) / "nested"
            source.write_text(code)
            subprocess.run(
                [
                    os.environ["CPPGNSS_CXX"],
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-pedantic",
                    "-I",
                    str(directory / "include"),
                    "-I",
                    os.environ["CPPGNSS_GENERATED_INCLUDE"],
                    str(source),
                    os.environ["CPPGNSS_LIBRARY"],
                    "-Wl,-rpath," + str(Path(os.environ["CPPGNSS_LIBRARY"]).parent),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_variant_discriminators_match_upstream(self):
        identities = {name: key for key, name in UBX_MSGIDS.items() if len(key) == 2}
        for name, variant in VARIANTS.items():
            selector = UPSTREAM_VARIANTS[GET].get(identities[variant.identity])
            if selector is None:  # SEC-UNIQID versions are documented in ubxtypes_get.
                continue
            with self.subTest(name=name):
                fields = parse_payload_def(UBX_PAYLOADS_GET[name])
                length = compute_struct_size(fields) if is_fixed_size(fields) else sum(f.size for f in fields if not f.is_repeating)
                payload = bytearray(length)
                if variant.offset is not None:
                    payload[variant.offset] = variant.value
                self.assertEqual(selector(payload=bytes(payload)), UBX_PAYLOADS_GET[name])


class NameCoverageTests(unittest.TestCase):
    def test_every_upstream_identity(self):
        classes, messages, subtypes = get_name_tables()
        for key, name in UBX_CLASSES.items():
            if name != "FOO":
                self.assertEqual(classes[key[0]], name)
        for key, name in UBX_MSGIDS.items():
            if name == "FOO-BAR":
                continue
            with self.subTest(name=name):
                table = messages if len(key) == 2 else subtypes
                self.assertEqual(table[int.from_bytes(key, "big")], name)
                self.assertIn(int.from_bytes(key[:2], "big"), messages)

    def test_mga_families_and_non_get_messages(self):
        _, messages, _ = get_name_tables()
        for key, name in {
            0x1300: "MGA-GPS",
            0x1302: "MGA-GAL",
            0x1303: "MGA-BDS",
            0x1305: "MGA-QZSS",
            0x1306: "MGA-GLO",
            0x1340: "MGA-INI",
            0x1360: "MGA-ACK/NAK",
            0x068A: "CFG-VALSET",
            0x068C: "CFG-VALDEL",
            0x0236: "RXM-SPARTN-KEY",
        }.items():
            self.assertEqual(messages[key], name)
        self.assertNotIn(0x6666, messages)
