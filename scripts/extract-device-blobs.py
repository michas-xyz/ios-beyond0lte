#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Extract and validate live DTB and Broadcom BCM4375 Wi-Fi/BT firmware from TWRP.

Connects via root ADB to a Samsung device running TWRP recovery, extracts:
1. Merged live device tree (/sys/firmware/fdt -> live.dtb)
2. BCM4375 Wi-Fi firmware (fw.bin, nvram.txt, clm.blob)
3. BCM4375 Bluetooth patch RAM (bt.hcd)
Validates headers, magic numbers, file sizes, and non-empty artifacts.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys

import b2l_board

FDT_MAGIC = 0xD00DFEED
WIFI_FW_MAX_BYTES = 2 * 1024 * 1024       # 2 MiB (B2L_WIFI_FW_CAP)
WIFI_NVRAM_MAX_BYTES = 64 * 1024          # 64 KiB (B2L_WIFI_NVRAM_CAP)
WIFI_CLM_MAX_BYTES = 64 * 1024            # 64 KiB (B2L_WIFI_CLM_CAP)
BT_HCD_MAX_BYTES = 256 * 1024             # 256 KiB (BT_HCD_CAP)

WIFI_FW_CANDIDATES = [
    "/vendor/etc/wifi/bcmdhd_sta.bin_b1",
    "/vendor/firmware/bcmdhd_sta.bin_b1",
    "/vendor/etc/wifi/bcmdhd_sta.bin",
    "/vendor/firmware/bcmdhd_sta.bin",
]

WIFI_NVRAM_CANDIDATES = [
    "/vendor/firmware/nvram.txt_CS01_semco_b1",
    "/vendor/etc/wifi/nvram.txt_CS01_semco_b1",
    "/vendor/etc/wifi/nvram_net.txt",
    "/vendor/etc/wifi/nvram.txt",
    "/vendor/firmware/nvram.txt",
]

WIFI_CLM_CANDIDATES = [
    "/vendor/etc/wifi/bcmdhd_clm.blob",
    "/vendor/firmware/bcmdhd_clm.blob",
]

BT_HCD_CANDIDATES = [
    "/vendor/firmware/bcm4375B1_semco.hcd",
    "/vendor/firmware/bcm4375B1.hcd",
    "/vendor/etc/bluetooth/bcm4375B1_semco.hcd",
    "/vendor/firmware/bcm4375.hcd",
]


class AdbRunner:
    """Execute ADB commands against the connected device."""

    def __init__(self, adb_path="adb", serial=None):
        self.adb = adb_path
        self.serial = serial

    def _base_cmd(self):
        cmd = [self.adb]
        if self.serial:
            cmd += ["-s", self.serial]
        return cmd

    def run(self, *args, check=True):
        cmd = self._base_cmd() + list(args)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if check and proc.returncode != 0:
            err = proc.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"ADB command failed ({' '.join(cmd)}): {err} (exit {proc.returncode})")
        return proc

    def get_state(self):
        proc = self.run("get-state", check=False)
        return proc.stdout.decode("utf-8", errors="replace").strip()

    def shell(self, cmd_str, check=True):
        proc = self.run("shell", cmd_str, check=check)
        return proc.stdout.decode("utf-8", errors="replace").strip()

    def pull_bytes(self, remote_path):
        """Pull binary data using cat via adb exec-out or shell."""
        proc = self.run("exec-out", f"cat '{remote_path}'", check=False)
        if proc.returncode != 0 or not proc.stdout:
            # Fallback to shell cat
            proc = self.run("shell", f"cat '{remote_path}'", check=True)
        return proc.stdout


def validate_fdt(data):
    """Validate that data is a valid Flattened Device Tree binary."""
    if len(data) < 40:
        raise ValueError(f"FDT data too short: {len(data)} bytes (expected >= 40)")
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic != FDT_MAGIC:
        raise ValueError(f"Invalid FDT magic: 0x{magic:08x} (expected 0x{FDT_MAGIC:08x})")
    totalsize = struct.unpack_from(">I", data, 4)[0]
    if totalsize > len(data):
        raise ValueError(f"Truncated FDT: header declared {totalsize} bytes, got {len(data)}")
    return totalsize


def validate_wifi_fw(data):
    """Validate BCM4375 Wi-Fi firmware blob."""
    if not data or len(data) < 1024:
        raise ValueError("Wi-Fi firmware blob is empty or unreasonably small")
    if len(data) > WIFI_FW_MAX_BYTES:
        raise ValueError(f"Wi-Fi firmware exceeds cap: {len(data)} > {WIFI_FW_MAX_BYTES}")
    return len(data)


def validate_wifi_nvram(data):
    """Validate BCM4375 NVRAM text configuration."""
    if not data or len(data) < 32:
        raise ValueError("Wi-Fi NVRAM is empty or unreasonably small")
    if len(data) > WIFI_NVRAM_MAX_BYTES:
        raise ValueError(f"Wi-Fi NVRAM exceeds cap: {len(data)} > {WIFI_NVRAM_MAX_BYTES}")
    text = data.decode("utf-8", errors="replace")
    if "=" not in text:
        raise ValueError("Wi-Fi NVRAM does not contain expected key=value pairs")
    return len(data)


def validate_wifi_clm(data):
    """Validate BCM4375 Country Locale Matrix blob."""
    if not data or len(data) < 16:
        raise ValueError("Wi-Fi CLM blob is empty or unreasonably small")
    if len(data) > WIFI_CLM_MAX_BYTES:
        raise ValueError(f"Wi-Fi CLM exceeds cap: {len(data)} > {WIFI_CLM_MAX_BYTES}")
    return len(data)


def validate_bt_hcd(data):
    """Validate Broadcom Bluetooth patch RAM (.hcd) file."""
    if not data or len(data) < 64:
        raise ValueError("Bluetooth HCD file is empty or unreasonably small")
    if len(data) > BT_HCD_MAX_BYTES:
        raise ValueError(f"Bluetooth HCD exceeds cap: {len(data)} > {BT_HCD_MAX_BYTES}")
    # Broadcom HCD format consists of HCI command packets:
    # 2 bytes Opcode, 1 byte param length, followed by params.
    # The first command in Broadcom patchram files is typically a vendor-specific command (e.g. 0xFCxx).
    offset = 0
    record_count = 0
    while offset + 3 <= len(data):
        opcode, length = struct.unpack_from("<HB", data, offset)
        offset += 3 + length
        record_count += 1
    if record_count == 0 or offset != len(data):
        raise ValueError(f"Malformed Bluetooth HCD stream: records ended at {offset}, total {len(data)}")
    return len(data)


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def find_remote_candidate(adb, candidates):
    """Find the first existing readable file from candidate remote paths."""
    for remote in candidates:
        res = adb.shell(f"[ -f '{remote}' ] && echo 1 || echo 0", check=False)
        if res.strip() == "1":
            return remote
    return None


def extract_device_blobs(adb, board="beyond0lte", output_dir=None,
                         skip_wifi=False, skip_bt=False, skip_dtb=False):
    """Orchestrate extraction of all firmware blobs from device."""
    expected_model = b2l_board.model(board)

    # 1. Verify recovery state
    state = adb.get_state()
    if state != "recovery":
        raise RuntimeError(f"Device not in recovery/TWRP. Current state: {state!r}")

    # 2. Verify root
    uid = adb.shell("id -u", check=False)
    if uid.strip() != "0":
        raise RuntimeError(f"Device shell is not running as root (uid={uid!r})")

    # 3. Verify hardware model
    model = adb.shell("getprop ro.boot.em.model", check=False)
    if model.strip() != expected_model:
        raise RuntimeError(f"Device model mismatch for board '{board}': "
                           f"expected {expected_model}, got {model.strip()}")

    out = Path(output_dir) if output_dir else Path("device-s10")
    dump_dir = out / "dump"
    wifi_bt_dir = out / "wifi-bt-fw/system"
    dump_dir.mkdir(parents=True, exist_ok=True)
    wifi_bt_dir.mkdir(parents=True, exist_ok=True)

    summary = {
        "board": board,
        "model": expected_model,
        "artifacts": {}
    }

    # 4. Extract Live DTB
    if not skip_dtb:
        print("Extracting live device tree from /sys/firmware/fdt...")
        dtb_data = adb.pull_bytes("/sys/firmware/fdt")
        validate_fdt(dtb_data)
        dtb_path = dump_dir / "live.dtb"
        dtb_path.write_bytes(dtb_data)
        h = sha256_hex(dtb_data)
        (dump_dir / "live.dtb.sha256").write_text(f"{h}  live.dtb\n")
        print(f"  OK: live.dtb ({len(dtb_data)} bytes, sha256: {h[:16]}...)")
        summary["artifacts"]["live.dtb"] = {"size": len(dtb_data), "sha256": h}

    # 5. Ensure /vendor is mounted
    if not skip_wifi or not skip_bt:
        mounts = adb.shell("cat /proc/mounts", check=False)
        if "/vendor " not in mounts and "/vendor/" not in mounts:
            print("Mounting /vendor read-only...")
            adb.shell("mount -o ro /dev/block/by-name/vendor /vendor", check=False)

    # 6. Extract Wi-Fi firmware
    if not skip_wifi:
        print("Extracting BCM4375 Wi-Fi firmware...")
        # fw.bin
        remote_fw = find_remote_candidate(adb, WIFI_FW_CANDIDATES)
        if not remote_fw:
            raise FileNotFoundError("Could not find Wi-Fi firmware blob on /vendor")
        fw_data = adb.pull_bytes(remote_fw)
        validate_wifi_fw(fw_data)
        (wifi_bt_dir / "fw.bin").write_bytes(fw_data)
        h_fw = sha256_hex(fw_data)
        print(f"  OK: fw.bin from {remote_fw} ({len(fw_data)} bytes, sha256: {h_fw[:16]}...)")
        summary["artifacts"]["fw.bin"] = {"size": len(fw_data), "sha256": h_fw, "source": remote_fw}

        # nvram.txt
        remote_nvram = find_remote_candidate(adb, WIFI_NVRAM_CANDIDATES)
        if not remote_nvram:
            raise FileNotFoundError("Could not find Wi-Fi NVRAM file on /vendor")
        nvram_data = adb.pull_bytes(remote_nvram)
        validate_wifi_nvram(nvram_data)
        (wifi_bt_dir / "nvram.txt").write_bytes(nvram_data)
        h_nvram = sha256_hex(nvram_data)
        print(f"  OK: nvram.txt from {remote_nvram} ({len(nvram_data)} bytes, sha256: {h_nvram[:16]}...)")
        summary["artifacts"]["nvram.txt"] = {"size": len(nvram_data), "sha256": h_nvram, "source": remote_nvram}

        # clm.blob
        remote_clm = find_remote_candidate(adb, WIFI_CLM_CANDIDATES)
        if not remote_clm:
            raise FileNotFoundError("Could not find Wi-Fi CLM blob on /vendor")
        clm_data = adb.pull_bytes(remote_clm)
        validate_wifi_clm(clm_data)
        (wifi_bt_dir / "clm.blob").write_bytes(clm_data)
        h_clm = sha256_hex(clm_data)
        print(f"  OK: clm.blob from {remote_clm} ({len(clm_data)} bytes, sha256: {h_clm[:16]}...)")
        summary["artifacts"]["clm.blob"] = {"size": len(clm_data), "sha256": h_clm, "source": remote_clm}

    # 7. Extract Bluetooth firmware
    if not skip_bt:
        print("Extracting BCM4375 Bluetooth patch RAM...")
        remote_bt = find_remote_candidate(adb, BT_HCD_CANDIDATES)
        if not remote_bt:
            raise FileNotFoundError("Could not find Bluetooth .hcd file on /vendor")
        bt_data = adb.pull_bytes(remote_bt)
        validate_bt_hcd(bt_data)
        (wifi_bt_dir / "bt.hcd").write_bytes(bt_data)
        h_bt = sha256_hex(bt_data)
        print(f"  OK: bt.hcd from {remote_bt} ({len(bt_data)} bytes, sha256: {h_bt[:16]}...)")
        summary["artifacts"]["bt.hcd"] = {"size": len(bt_data), "sha256": h_bt, "source": remote_bt}

    print("\nExtraction complete! All artifacts verified non-empty and valid.")
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--board", choices=sorted(b2l_board.BOARDS.keys()),
                   default=os.environ.get("B2L_BOARD", "beyond0lte"),
                   help="Target board (default: beyond0lte or $B2L_BOARD)")
    p.add_argument("--adb", default=os.environ.get("ADB", "adb"),
                   help="Path to ADB binary (default: $ADB or 'adb')")
    p.add_argument("--serial", default=os.environ.get("B2L_SERIAL"),
                   help="Device serial number (default: $B2L_SERIAL)")
    p.add_argument("--output-dir", type=Path, default=None,
                   help="Target output directory (default: device-s10)")
    p.add_argument("--skip-wifi", action="store_true", help="Skip Wi-Fi firmware extraction")
    p.add_argument("--skip-bt", action="store_true", help="Skip Bluetooth firmware extraction")
    p.add_argument("--skip-dtb", action="store_true", help="Skip live DTB extraction")
    a = p.parse_args()

    adb = AdbRunner(adb_path=a.adb, serial=a.serial)
    try:
        extract_device_blobs(adb, board=a.board, output_dir=a.output_dir,
                             skip_wifi=a.skip_wifi, skip_bt=a.skip_bt,
                             skip_dtb=a.skip_dtb)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
