#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Unit tests for scripts/make-board-platform.py."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

B2L_PLATFORM_GUID = '556BDA6E-EBC8-4D8F-8EA6-DAA2D7AD7A68'
B0L_PLATFORM_GUID = '4F92A8E1-2856-4BD3-A0A1-3C389E8D970F'
B1L_PLATFORM_GUID = '4E670446-C67E-4FEB-8D15-A0A05C824B80'


def make_mock_fdt(board_magic: bytes) -> bytes:
    """Generate a minimal valid FDT header with embedded board magic."""
    header = bytearray(64)
    struct.pack_into(">I", header, 0, 0xD00DFEED)  # magic
    struct.pack_into(">I", header, 4, 64)          # totalsize
    struct.pack_into(">I", header, 8, 48)          # off_dt_struct
    struct.pack_into(">I", header, 12, 56)         # off_dt_strings
    struct.pack_into(">I", header, 16, 40)         # off_mem_rsvmap
    struct.pack_into(">I", header, 20, 17)         # version
    struct.pack_into(">I", header, 24, 16)         # last_comp_version
    # Put magic in strings section
    header[40:40 + len(board_magic)] = board_magic
    return bytes(header)


def setup_mock_mu_tree(root: Path):
    """Create a minimal synthetic Mu-Silicium workspace."""
    (root / "Mu_Basecore").mkdir(parents=True)
    pkg = root / "Platforms/Samsung/beyond2ltePkg"
    pkg.mkdir(parents=True)

    dsc_content = f"""[Defines]
  PLATFORM_NAME                  = beyond2lte
  PLATFORM_GUID                  = {B2L_PLATFORM_GUID}
  FLASH_DEFINITION               = beyond2ltePkg/beyond2lte.fdf
  OUTPUT_DIRECTORY               = Build/beyond2lte
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE

[PcdsFixedAtBuild]
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Samsung"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Galaxy S10+"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemSku|"Galaxy_S10+_beyond2lte"
  gSiliciumPkgTokenSpaceGuid.PcdDevicePlatform|"beyond2lte"
"""
    (pkg / "beyond2lte.dsc").write_text(dsc_content)
    (pkg / "beyond2lte.fdf").write_text("[FV.FVMAIN_COMPACT]\n# beyond2ltePkg/beyond2lte.dsc\n")
    (pkg / "beyond2lte.dec").write_text("[Defines]\nPACKAGE_NAME = beyond2ltePkg\n")
    (pkg / "DeviceBuild.py").write_text("BOARD = 'beyond2lte'\nPKG = 'beyond2ltePkg'\n")
    (pkg / "Include").mkdir()
    (pkg / "Include/Board.h").write_text("#define BOARD_NAME \"samsung-beyond2lte\"\n")

    configs = root / "Resources/Configs"
    configs.mkdir(parents=True)
    (configs / "beyond2lte.toml").write_text("board = 'beyond2lte'\n")


class MakeBoardPlatformTests(unittest.TestCase):
    def test_generate_beyond0lte_platform(self):
        with tempfile.TemporaryDirectory() as td:
            mu_root = Path(td) / "mu"
            setup_mock_mu_tree(mu_root)

            dtb_path = Path(td) / "beyond0lte.dtb"
            dtb_path.write_bytes(make_mock_fdt(b"BEYOND0LTE"))

            cmd = [
                sys.executable,
                str(ROOT / "scripts/make-board-platform.py"),
                "--board", "beyond0lte",
                "--mu", str(mu_root),
                "--dtb", str(dtb_path)
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(res.returncode, 0, f"make-board-platform failed: {res.stderr}")

            target = mu_root / "Platforms/Samsung/beyond0ltePkg"
            self.assertTrue(target.is_dir())
            self.assertTrue((target / "beyond0lte.dsc").is_file())
            self.assertTrue((target / "beyond0lte.fdf").is_file())
            self.assertTrue((target / "beyond0lte.dec").is_file())
            self.assertFalse((target / "beyond2lte.dsc").exists())

            dsc_text = (target / "beyond0lte.dsc").read_text()
            self.assertIn(B0L_PLATFORM_GUID, dsc_text)
            self.assertIn("PLATFORM_NAME                  = beyond0lte", dsc_text)
            self.assertIn("beyond0ltePkg/beyond0lte.fdf", dsc_text)
            self.assertIn('"Galaxy S10e"', dsc_text)
            self.assertIn('"Galaxy_S10e_beyond0lte"', dsc_text)

            # Check that no beyond2lte leftovers remain in build files
            for p in target.rglob("*"):
                if p.is_file() and p.suffix in {'.dsc', '.fdf', '.inf', '.inc', '.py'}:
                    self.assertNotIn('beyond2lte', p.read_text(), f"Leftover in {p}")

            # Check configs and DTBs
            self.assertTrue((mu_root / "Resources/Configs/beyond0lte.toml").is_file())
            self.assertTrue((mu_root / "Resources/DTBs/beyond0lte.dtb").is_file())
            self.assertEqual((mu_root / "Resources/DTBs/beyond0lte.dtb").read_bytes(), dtb_path.read_bytes())

    def test_generate_beyond1lte_backwards_compatibility(self):
        with tempfile.TemporaryDirectory() as td:
            mu_root = Path(td) / "mu"
            setup_mock_mu_tree(mu_root)

            dtb_path = Path(td) / "beyond1lte.dtb"
            dtb_path.write_bytes(make_mock_fdt(b"BEYOND1LTE"))

            cmd = [
                sys.executable,
                str(ROOT / "scripts/make-board-platform.py"),
                "--board", "beyond1lte",
                "--mu", str(mu_root),
                "--dtb", str(dtb_path)
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(res.returncode, 0, f"make-board-platform failed: {res.stderr}")

            target = mu_root / "Platforms/Samsung/beyond1ltePkg"
            self.assertTrue(target.is_dir())
            dsc_text = (target / "beyond1lte.dsc").read_text()
            self.assertIn(B1L_PLATFORM_GUID, dsc_text)
            self.assertIn("PLATFORM_NAME                  = beyond1lte", dsc_text)
            self.assertIn('"Galaxy S10"', dsc_text)
            self.assertIn('"Galaxy_S10_beyond1lte"', dsc_text)

    def test_dtb_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            mu_root = Path(td) / "mu"
            setup_mock_mu_tree(mu_root)

            # Passing a DTB with BEYOND1LTE when targeting beyond0lte must fail
            dtb_path = Path(td) / "wrong.dtb"
            dtb_path.write_bytes(make_mock_fdt(b"BEYOND1LTE"))

            cmd = [
                sys.executable,
                str(ROOT / "scripts/make-board-platform.py"),
                "--board", "beyond0lte",
                "--mu", str(mu_root),
                "--dtb", str(dtb_path)
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("does not describe beyond0lte", res.stderr)

    def test_invalid_fdt_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            mu_root = Path(td) / "mu"
            setup_mock_mu_tree(mu_root)

            dtb_path = Path(td) / "bad.dtb"
            dtb_path.write_bytes(b"NOT_A_DEVICE_TREE_AT_ALL_SHORT")

            cmd = [
                sys.executable,
                str(ROOT / "scripts/make-board-platform.py"),
                "--board", "beyond0lte",
                "--mu", str(mu_root),
                "--dtb", str(dtb_path)
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("not a flattened device tree", res.stderr)


if __name__ == '__main__':
    unittest.main()
