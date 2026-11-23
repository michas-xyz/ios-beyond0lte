#!/usr/bin/env python3
"""Stream and decrypt IPSW DMGs directly from Apple CDN without storing the 11GB IPSW.

Downloads:
  - AppOS.dmg (from 094-96441-094.dmg, ~14MB)
  - SystemOS.dmg (from 094-96846-092.dmg.aea, ~2.0GB -> ~4.9GB)
  - OS.dmg (from 094-95611-090.dmg.aea, ~8.3GB -> ~9.0GB)
"""
import base64
import os
import struct
import subprocess
import sys
import time
import urllib.request
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
AEA_KEY_PY = os.path.join(SCRIPT_DIR, 'aea-key.py')
IPSW_URL = 'https://updates.cdn-apple.com/2026SummerFCS/a1dbd14a-9e9d-49b3-9af6-02286d68883f/iPhone17,2_26.6.2_23G90_Restore.ipsw'
TOTAL_SIZE = 11360650339

# Pinned offsets in the IPSW zip
FILES = {
    'AppOS': {
        'name': '094-96441-094.dmg',
        'local_offset': 8661371609,
        'comp_size': 7254358,
        'uncomp_size': 14680064,
        'method': 8,
        'encrypted': False,
        'out_name': 'AppOS.dmg',
    },
    'SystemOS': {
        'name': '094-96846-092.dmg.aea',
        'local_offset': 8668626034,
        'comp_size': 2013265920,
        'uncomp_size': 2013265920,
        'method': 0,
        'encrypted': True,
        'out_name': 'SystemOS.dmg',
    },
    'OS': {
        'name': '094-95611-090.dmg.aea',
        'local_offset': 184683083,
        'comp_size': 8321499136,
        'uncomp_size': 8321499136,
        'method': 0,
        'encrypted': True,
        'out_name': 'OS.dmg',
    },
}


def get_data_offset(url, local_offset):
    req = urllib.request.Request(url, headers={'Range': f'bytes={local_offset}-{local_offset + 128}'})
    with urllib.request.urlopen(req, timeout=15) as resp:
        hdr = resp.read()
    if hdr[:4] != b'PK\x03\x04':
        raise ValueError(f'Bad local header signature: {hdr[:4]}')
    fn_len, extra_len = struct.unpack('<HH', hdr[26:30])
    return local_offset + 30 + fn_len + extra_len


def download_range(url, start_byte, size, out_file, chunk_size=4*1024*1024):
    end_byte = start_byte + size - 1
    req = urllib.request.Request(url, headers={'Range': f'bytes={start_byte}-{end_byte}'})
    downloaded = 0
    t0 = time.time()
    last_p = t0
    with urllib.request.urlopen(req, timeout=30) as resp:
        while downloaded < size:
            to_read = min(chunk_size, size - downloaded)
            chunk = resp.read(to_read)
            if not chunk:
                break
            out_file.write(chunk)
            downloaded += len(chunk)
            now = time.time()
            if now - last_p >= 2.0 or downloaded >= size:
                pct = (downloaded / size) * 100
                speed = (downloaded / (1024*1024)) / (now - t0 + 0.001)
                print(f'    [{pct:5.1f}%] {downloaded/(1024**2):,.1f}/{size/(1024**2):,.1f} MiB ({speed:.1f} MiB/s)', flush=True)
                last_p = now


def fetch_file(target_key, out_dir):
    info = FILES[target_key]
    out_path = os.path.join(out_dir, info['out_name'])
    if os.path.exists(out_path):
        print(f'{info["out_name"]} already exists at {out_path}, skipping download.')
        return out_path

    data_offset = get_data_offset(IPSW_URL, info['local_offset'])
    print(f'Fetching {target_key} ({info["name"]}) at offset {data_offset:,} (size: {info["comp_size"]:,} bytes)...')

    if not info['encrypted']:
        # Deflated dmg (AppOS)
        temp_comp = os.path.join(out_dir, info['name'] + '.comp')
        with open(temp_comp, 'wb') as f:
            download_range(IPSW_URL, data_offset, info['comp_size'], f)
        print(f'  Decompressing raw Deflate payload to {out_path}...')
        with open(temp_comp, 'rb') as fin, open(out_path, 'wb') as fout:
            decompressor = zlib.decompressobj(-zlib.MAX_WBITS)
            while chunk := fin.read(1024*1024):
                fout.write(decompressor.decompress(chunk))
            fout.write(decompressor.flush())
        os.remove(temp_comp)
        print(f'  {info["out_name"]} ready: {os.path.getsize(out_path):,} bytes.')
        return out_path

    # Encrypted AEA: fetch header to obtain key, then download and decrypt
    header_path = os.path.join(out_dir, info['name'] + '.head')
    req = urllib.request.Request(IPSW_URL, headers={'Range': f'bytes={data_offset}-{data_offset + 65535}'})
    with urllib.request.urlopen(req, timeout=15) as resp, open(header_path, 'wb') as hf:
        hf.write(resp.read())

    key_res = subprocess.run([sys.executable, AEA_KEY_PY, header_path], capture_output=True, text=True, check=True)
    key_str = key_res.stdout.strip()
    os.remove(header_path)
    print(f'  Retrieved decryption key: {key_str}')

    # Download AEA stream
    aea_path = os.path.join(out_dir, info['name'])
    print(f'  Downloading AEA image ({info["comp_size"] / (1024**3):.2f} GiB)...')
    with open(aea_path, 'wb') as f:
        download_range(IPSW_URL, data_offset, info['comp_size'], f)

    # Decrypt
    print(f'  Decrypting AEA -> {out_path}...')
    # Check if aea CLI is available (on macOS)
    aea_bin = subprocess.run(['which', 'aea'], capture_output=True, text=True).stdout.strip()
    if aea_bin and os.path.exists(aea_bin):
        subprocess.run(['aea', 'decrypt', '-i', aea_path, '-o', out_path, '-key-value', key_str], check=True)
    else:
        # Use python decryptor
        aea_dec_py = os.path.join(SCRIPT_DIR, 'aea_decrypt.py')
        subprocess.run([sys.executable, '-u', aea_dec_py, aea_path, out_path, key_str], check=True)

    os.remove(aea_path)
    print(f'  {info["out_name"]} decrypted and ready: {os.path.getsize(out_path):,} bytes.')
    return out_path


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else 'ipsw-23G90'
    os.makedirs(out_dir, exist_ok=True)
    target = sys.argv[2] if len(sys.argv) > 2 else 'all'

    if target == 'all':
        for k in ['AppOS', 'SystemOS', 'OS']:
            fetch_file(k, out_dir)
    else:
        fetch_file(target, out_dir)


if __name__ == '__main__':
    main()
