#!/usr/bin/env python3
"""Tests for scripts/generate-metal-payload.py."""
import importlib.util
from pathlib import Path
import re
import struct
import tempfile
import unittest
import zlib


SPEC = importlib.util.spec_from_file_location(
    "generate_metal_payload",
    Path(__file__).resolve().parents[1] / "scripts/generate-metal-payload.py",
)
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


class GenerateMetalPayloadTests(unittest.TestCase):
    def make_macho(self, payload: bytes) -> bytes:
        hdr = struct.pack("<IiiIIIII", 0xfeedfacf, 0x0100000c, 2, 6, 0, 0, 0, 0)
        return hdr + payload

    def test_generates_valid_compressed_header_and_roundtrips(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inp = root / "in"
            inp.mkdir()
            expected = {
                "b2l_metal_bundle_bin": self.make_macho(b"bundle" * 64),
                "b2l_metal_bundle_plist": b"<plist><dict><key>NSPrincipalClass</key><string>B2LMetalUIDevice</string></dict></plist>",
                "b2l_metal_libhakiosmetal": self.make_macho(b"metal" * 128),
                "b2l_metal_libhakiosair": self.make_macho(b"panfrost" * 512),
            }
            for fname, sym, _ in MOD.FILES:
                (inp / fname).write_bytes(expected[sym])
            out = root / "b2l_metal_payload.h"
            MOD.generate(inp, out)
            text = out.read_text(encoding="utf-8")
            self.assertIn("#define B2L_METAL_EMBEDDED 1", text)
            for _, sym, _ in MOD.FILES:
                m_len = re.search(rf"static const size_t {sym}_raw_len = (\d+)u;", text)
                self.assertIsNotNone(m_len)
                raw_len = int(m_len.group(1))
                self.assertEqual(raw_len, len(expected[sym]))
                m_arr = re.search(rf"static const unsigned char {sym}_z\[\] = \{{([^}}]+)\}};", text, re.S)
                self.assertIsNotNone(m_arr)
                zbytes = bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]{2}", m_arr.group(1)))
                self.assertEqual(zlib.decompress(zbytes), expected[sym])

    def test_rejects_non_arm64_macho(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inp = root / "in"
            inp.mkdir()
            for fname, _, is_macho in MOD.FILES:
                (inp / fname).write_bytes(b"bad" * 16 if is_macho else b"<plist>B2LMetalUIDevice</plist>")
            with self.assertRaises(ValueError):
                MOD.generate(inp, root / "out.h")


if __name__ == "__main__":
    unittest.main()
