#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Append a local B2LFW001 Wi-Fi/BT firmware trailer to a verified boot.img.

Keeps proprietary device blobs out of Git while letting b2l_ufs_disk.c load
fw.bin, nvram.txt, clm.blob, wifi.conf and bt.hcd from the BOOT partition.
"""
import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

MAGIC = b"B2LFW001"
HEADER_BYTES = 64 + 16 * 32  # 576 bytes


def align4096(n):
    return (n + 4095) & ~4095


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--boot-img", type=pathlib.Path, required=True)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--fw", type=pathlib.Path, required=True)
    ap.add_argument("--nvram", type=pathlib.Path, required=True)
    ap.add_argument("--clm", type=pathlib.Path, required=True)
    ap.add_argument("--bt-hcd", type=pathlib.Path, required=True)
    ap.add_argument("--wifi-mac", help="Colon-separated Wi-Fi MAC address")
    ap.add_argument("--country", default="DE")
    ap.add_argument("--max-bytes", type=int, default=57671680)
    a = ap.parse_args()

    import importlib.util
    pb_spec = importlib.util.spec_from_file_location("package_boot", pathlib.Path(__file__).with_name("package-boot.py"))
    pb = importlib.util.module_from_spec(pb_spec)
    pb_spec.loader.exec_module(pb)

    info = pb.check_boot(a.boot_img)
    raw = bytearray(a.boot_img.read_bytes())

    if a.wifi_mac:
        mac = bytes.fromhex(a.wifi_mac.replace(":", ""))
        meta = pb.wifi_metadata(mac)
        trailer = info["trailer_file_offset"]
        raw[trailer + 288:trailer + 320] = meta

    # Truncate any previous B2LFW001 trailer beyond XNU payload
    base_end = info["trailer_file_offset"] + pb.HEADER_SIZE + info["xnu_bytes"]
    base_len = align4096(max(len(raw), base_end))
    if len(raw) < base_len:
        raw.extend(b"\x00" * (base_len - len(raw)))

    conf_lines = []
    if a.wifi_mac:
        conf_lines.append(f"mac={a.wifi_mac.lower()}")
    if a.country:
        conf_lines.append(f"country={a.country.upper()}")
    conf_bytes = ("\n".join(conf_lines) + "\n").encode("ascii")

    items = [
        ("fw.bin", a.fw.read_bytes()),
        ("nvram.txt", a.nvram.read_bytes()),
        ("clm.blob", a.clm.read_bytes()),
        ("bt.hcd", a.bt_hcd.read_bytes()),
        ("wifi.conf", conf_bytes),
    ]

    off = align4096(HEADER_BYTES)
    entries = []
    blob = bytearray(off)
    for name, payload in items:
        entries.append((name.encode("ascii"), off, len(payload), zlib.crc32(payload) & 0xffffffff))
        blob.extend(payload)
        pad = (-len(payload)) & 4095
        if pad:
            blob.extend(b"\x00" * pad)
        off += len(payload) + pad

    struct.pack_into("<8sIIQ40s", blob, 0, MAGIC, 1, len(entries), len(blob), b"\x00" * 40)
    for i, (nm, eoff, esz, ecrc) in enumerate(entries):
        struct.pack_into("<16sIIII", blob, 64 + i * 32, nm, eoff, esz, ecrc, 0)

    out_bytes = bytes(raw[:base_len]) + bytes(blob)
    if len(out_bytes) > a.max_bytes:
        raise SystemExit(f"boot image too large: {len(out_bytes)} > {a.max_bytes}")

    a.output.write_bytes(out_bytes)
    pb.check_boot(a.output)
    print(f"Wrote {a.output} ({len(out_bytes)} bytes), sha256={hashlib.sha256(out_bytes).hexdigest()}")


if __name__ == "__main__":
    main()
