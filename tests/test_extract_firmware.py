#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Unit and regression tests for scripts/extract-device-blobs.py."""
from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import importlib.util
spec = importlib.util.spec_from_file_location('extractor', ROOT / 'scripts/extract-device-blobs.py')
extractor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extractor)


def create_synthetic_fdt(totalsize=256) -> bytes:
    header = bytearray(totalsize)
    struct.pack_into(">I", header, 0, extractor.FDT_MAGIC)
    struct.pack_into(">I", header, 4, totalsize)
    struct.pack_into(">I", header, 8, 48)   # off_dt_struct
    struct.pack_into(">I", header, 12, 56)  # off_dt_strings
    struct.pack_into(">I", header, 16, 40)  # off_mem_rsvmap
    struct.pack_into(">I", header, 20, 17)  # version
    struct.pack_into(">I", header, 24, 16)  # last_comp_version
    return bytes(header)


def create_synthetic_hcd() -> bytes:
    """Create a minimal valid HCD packet stream: opcode(2), length(1), data(len)."""
    stream = bytearray()
    # Packet 1: Opcode 0xFC01, len 4, params 0x01 0x02 0x03 0x04
    stream.extend(struct.pack("<HB", 0xFC01, 4) + b"\x01\x02\x03\x04")
    # Packet 2: Opcode 0xFC18, len 64, params
    stream.extend(struct.pack("<HB", 0xFC18, 64) + bytes(range(64)))
    # Repeat to ensure > 64 bytes
    return bytes(stream)


class MockAdbRunner:
    """Mock ADB runner for testing extraction without a physical phone."""

    def __init__(self, state="recovery", uid="0", model="SM-G970F",
                 files=None, vendor_mounted=True):
        self.state = state
        self.uid = uid
        self.model = model
        self.vendor_mounted = vendor_mounted
        self.files = files or {}
        self.commands = []

    def get_state(self):
        return self.state

    def shell(self, cmd_str, check=True):
        self.commands.append(("shell", cmd_str))
        if cmd_str == "id -u":
            return self.uid
        if cmd_str == "getprop ro.boot.em.model":
            return self.model
        if cmd_str == "cat /proc/mounts":
            return "/dev/block/sda26 /vendor ext4 ro 0 0" if self.vendor_mounted else ""
        prefix = "[ -f '"
        suffix = "' ] && echo 1 || echo 0"
        if cmd_str.startswith(prefix) and cmd_str.endswith(suffix):
            path = cmd_str[len(prefix):-len(suffix)]
            return "1" if path in self.files else "0"
        if cmd_str.startswith("mount "):
            self.vendor_mounted = True
            return ""
        return ""

    def pull_bytes(self, remote_path):
        self.commands.append(("pull", remote_path))
        if remote_path not in self.files:
            raise RuntimeError(f"Mock file not found: {remote_path}")
        return self.files[remote_path]


class ExtractFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.valid_dtb = create_synthetic_fdt(512)
        self.valid_fw = b"BCM4375_WIFI_FIRMWARE_PAYLOAD_" + bytes(range(256)) * 8  # > 2KB
        self.valid_nvram = b"sromrev=11\nboardtype=0x088f\nboardrev=0x1122\n"
        self.valid_clm = b"BCM4375_CLM_DATA_" + bytes(64)
        self.valid_hcd = create_synthetic_hcd()

    def test_validators_with_valid_data(self):
        self.assertEqual(extractor.validate_fdt(self.valid_dtb), 512)
        self.assertGreater(extractor.validate_wifi_fw(self.valid_fw), 1024)
        self.assertGreater(extractor.validate_wifi_nvram(self.valid_nvram), 30)
        self.assertGreater(extractor.validate_wifi_clm(self.valid_clm), 16)
        self.assertGreater(extractor.validate_bt_hcd(self.valid_hcd), 64)

    def test_validators_reject_invalid_data(self):
        # Truncated FDT
        with self.assertRaises(ValueError):
            extractor.validate_fdt(b"SHORT")
        # Wrong magic FDT
        with self.assertRaises(ValueError):
            extractor.validate_fdt(b"\x00" * 48)

        # Empty / short Wi-Fi FW
        with self.assertRaises(ValueError):
            extractor.validate_wifi_fw(b"too small")
        # Oversized Wi-Fi FW
        with self.assertRaises(ValueError):
            extractor.validate_wifi_fw(bytes(3 * 1024 * 1024))

        # NVRAM without key=val
        with self.assertRaises(ValueError):
            extractor.validate_wifi_nvram(b"some text without equal sign in it")

        # Corrupted HCD stream
        with self.assertRaises(ValueError):
            extractor.validate_bt_hcd(b"\x01\x02\x03\x04\x05")

    def test_successful_extraction_beyond0lte(self):
        mock_files = {
            "/sys/firmware/fdt": self.valid_dtb,
            "/vendor/etc/wifi/bcmdhd_sta.bin_b1": self.valid_fw,
            "/vendor/etc/wifi/nvram.txt_CS01_semco_b1": self.valid_nvram,
            "/vendor/etc/wifi/bcmdhd_clm.blob": self.valid_clm,
            "/vendor/firmware/bcm4375B1_semco.hcd": self.valid_hcd,
        }
        mock_adb = MockAdbRunner(state="recovery", uid="0", model="SM-G970F", files=mock_files)

        with tempfile.TemporaryDirectory() as td:
            out_dir = Path(td) / "device-s10"
            summary = extractor.extract_device_blobs(
                mock_adb, board="beyond0lte", output_dir=out_dir
            )

            self.assertEqual(summary["board"], "beyond0lte")
            self.assertEqual(summary["model"], "SM-G970F")

            # Check files created and verified
            dtb_file = out_dir / "dump/live.dtb"
            self.assertTrue(dtb_file.is_file())
            self.assertEqual(dtb_file.read_bytes(), self.valid_dtb)
            self.assertTrue((out_dir / "dump/live.dtb.sha256").is_file())

            fw_dir = out_dir / "wifi-bt-fw/system"
            self.assertEqual((fw_dir / "fw.bin").read_bytes(), self.valid_fw)
            self.assertEqual((fw_dir / "nvram.txt").read_bytes(), self.valid_nvram)
            self.assertEqual((fw_dir / "clm.blob").read_bytes(), self.valid_clm)
            self.assertEqual((fw_dir / "bt.hcd").read_bytes(), self.valid_hcd)

    def test_error_when_device_not_in_recovery(self):
        mock_adb = MockAdbRunner(state="device")
        with self.assertRaises(RuntimeError) as ctx:
            extractor.extract_device_blobs(mock_adb, board="beyond0lte")
        self.assertIn("not in recovery/TWRP", str(ctx.exception))

    def test_error_when_device_not_root(self):
        mock_adb = MockAdbRunner(uid="2000")
        with self.assertRaises(RuntimeError) as ctx:
            extractor.extract_device_blobs(mock_adb, board="beyond0lte")
        self.assertIn("not running as root", str(ctx.exception))

    def test_error_when_model_mismatch(self):
        # Targeting beyond0lte (SM-G970F) but device reports SM-G973F (beyond1lte)
        mock_adb = MockAdbRunner(model="SM-G973F")
        with self.assertRaises(RuntimeError) as ctx:
            extractor.extract_device_blobs(mock_adb, board="beyond0lte")
        self.assertIn("Device model mismatch", str(ctx.exception))

    def test_error_when_missing_vendor_files(self):
        # Only DTB available, missing Wi-Fi firmware
        mock_files = {
            "/sys/firmware/fdt": self.valid_dtb,
        }
        mock_adb = MockAdbRunner(state="recovery", uid="0", model="SM-G970F", files=mock_files)
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(FileNotFoundError) as ctx:
                extractor.extract_device_blobs(mock_adb, board="beyond0lte", output_dir=td)
            self.assertIn("Wi-Fi firmware blob", str(ctx.exception))


class FlashKitShellRegressionTests(unittest.TestCase):
    """Regression tests ensuring flash-kit shell scripts avoid Bash reserved variable collisions."""

    FLASH_KIT_DIR = ROOT / "tools" / "flash-kit"
    TARGET_SCRIPTS = [
        "check-device.sh",
        "flash-images.sh",
        "backup-partitions.sh",
    ]

    def test_scripts_exist(self):
        for script_name in self.TARGET_SCRIPTS:
            script_path = self.FLASH_KIT_DIR / script_name
            self.assertTrue(script_path.is_file(), f"Missing flash-kit script: {script_name}")

    def test_no_uid_readonly_variable_assignment(self):
        """Verify no shell script in tools/flash-kit assigns to GNU Bash's read-only UID variable."""
        uid_assign_pattern = re.compile(r'^\s*UID\s*=', re.MULTILINE)
        for script_name in self.TARGET_SCRIPTS:
            script_path = self.FLASH_KIT_DIR / script_name
            content = script_path.read_text(encoding="utf-8")
            matches = uid_assign_pattern.findall(content)
            self.assertEqual(
                matches,
                [],
                f"Forbidden readonly variable assignment 'UID=' detected in {script_name}",
            )
            # Ensure DEVICE_UID is used instead
            self.assertIn(
                "DEVICE_UID=",
                content,
                f"Expected 'DEVICE_UID=' assignment not found in {script_name}",
            )

    def test_bash_syntax(self):
        """Check bash -n syntax on all tools/flash-kit shell scripts if bash is installed."""
        bash_path = shutil.which("bash")
        if not bash_path:
            self.skipTest("bash not found on system PATH")

        for sh_file in self.FLASH_KIT_DIR.glob("*.sh"):
            result = subprocess.run(
                [bash_path, "-n", str(sh_file)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"Syntax error in {sh_file.name}:\n{result.stderr}",
            )

    def test_check_device_execution_under_bash(self):
        """Test check-device.sh runs cleanly under Bash with mock ADB without UID collision."""
        bash_path = shutil.which("bash")
        if not bash_path:
            self.skipTest("bash not found on system PATH")

        with tempfile.TemporaryDirectory() as td:
            mock_adb_script = Path(td) / "mock_adb.sh"
            mock_adb_script.write_text(
                "#!/bin/bash\n"
                "if [ \"$1\" = \"get-state\" ]; then\n"
                "    echo \"recovery\"\n"
                "    exit 0\n"
                "fi\n"
                "if [ \"$1\" = \"shell\" ]; then\n"
                "    shift\n"
                "    subcmd=\"$*\"\n"
                "    if [ \"$subcmd\" = \"id -u\" ]; then\n"
                "        echo \"${MOCK_UID:-0}\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [ \"$subcmd\" = \"getprop ro.boot.em.model\" ]; then\n"
                "        echo \"${MOCK_MODEL:-SM-G970F}\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"readlink -f /dev/block/by-name/boot\"* ]]; then\n"
                "        echo \"/dev/block/sda14\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"readlink -f /dev/block/by-name/system\"* ]]; then\n"
                "        echo \"/dev/block/sda25\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"readlink -f /dev/block/by-name/userdata\"* ]]; then\n"
                "        echo \"/dev/block/sda34\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"blockdev --getsize64 /dev/block/sda14\"* ]]; then\n"
                "        echo \"57671680\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"blockdev --getsize64 /dev/block/sda25\"* ]]; then\n"
                "        echo \"7340032000\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"blockdev --getsize64 /dev/block/sda34\"* ]]; then\n"
                "        echo \"115641200640\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [[ \"$subcmd\" == *\"grep -q\"* ]]; then\n"
                "        echo \"libre\"\n"
                "        exit 0\n"
                "    fi\n"
                "fi\n"
                "echo \"\"\n"
                "exit 0\n",
                encoding="utf-8",
            )
            mock_adb_posix = str(mock_adb_script).replace("\\", "/")

            # Case 1: Success for beyond0lte
            env = os.environ.copy()
            env["ADB"] = f"bash {mock_adb_posix}"
            env["B2L_BOARD"] = "beyond0lte"
            env["MOCK_UID"] = "0"
            env["MOCK_MODEL"] = "SM-G970F"

            res = subprocess.run(
                [bash_path, str(self.FLASH_KIT_DIR / "check-device.sh")],
                env=env,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertNotIn("readonly variable", res.stderr)
            self.assertEqual(res.returncode, 0, f"check-device.sh failed:\nstdout: {res.stdout}\nstderr: {res.stderr}")
            self.assertIn("modèle SM-G970F (beyond0lte) confirmé", res.stdout)
            self.assertIn("TOUTES LES VÉRIFICATIONS SONT OK", res.stdout)

            # Case 2: Non-root detection
            env["MOCK_UID"] = "2000"
            res = subprocess.run(
                [bash_path, str(self.FLASH_KIT_DIR / "check-device.sh")],
                env=env,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("le téléphone n'est pas root (uid=2000)", res.stdout)
            self.assertNotIn("readonly variable", res.stderr)

    def test_backup_and_flash_root_checks_under_bash(self):
        """Test backup-partitions.sh and flash-images.sh execute root checks without UID collisions."""
        bash_path = shutil.which("bash")
        if not bash_path:
            self.skipTest("bash not found on system PATH")

        with tempfile.TemporaryDirectory() as td:
            mock_adb_script = Path(td) / "mock_adb.sh"
            mock_adb_script.write_text(
                "#!/bin/bash\n"
                "if [ \"$1\" = \"get-state\" ]; then\n"
                "    echo \"recovery\"\n"
                "    exit 0\n"
                "fi\n"
                "if [ \"$1\" = \"shell\" ]; then\n"
                "    shift\n"
                "    if [ \"$*\" = \"id -u\" ]; then\n"
                "        echo \"${MOCK_UID:-2000}\"\n"
                "        exit 0\n"
                "    fi\n"
                "    if [ \"$*\" = \"getprop ro.boot.em.model\" ]; then\n"
                "        echo \"SM-G970F\"\n"
                "        exit 0\n"
                "    fi\n"
                "fi\n"
                "exit 0\n",
                encoding="utf-8",
            )
            mock_adb_posix = str(mock_adb_script).replace("\\", "/")
            env = os.environ.copy()
            env["ADB"] = f"bash {mock_adb_posix}"
            env["MOCK_UID"] = "2000"

            # backup-partitions.sh fails cleanly on non-root
            res_backup = subprocess.run(
                [bash_path, str(self.FLASH_KIT_DIR / "backup-partitions.sh")],
                env=env,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertNotIn("readonly variable", res_backup.stderr)
            self.assertNotEqual(res_backup.returncode, 0)
            self.assertIn("le téléphone n'est pas root (uid=2000)", res_backup.stdout)

            # flash-images.sh fails cleanly on non-root
            res_flash = subprocess.run(
                [bash_path, str(self.FLASH_KIT_DIR / "flash-images.sh")],
                env=env,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertNotIn("readonly variable", res_flash.stderr)
            self.assertNotEqual(res_flash.returncode, 0)
            self.assertIn("pas root (uid=2000)", res_flash.stdout)


if __name__ == '__main__':
    unittest.main()
