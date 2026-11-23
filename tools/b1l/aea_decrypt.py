#!/usr/bin/env python3
"""Decrypt Apple Encrypted Archive (AEA1) Profile 1 images using native LZFSE.

Optimized for Windows with lzfse-win64.dll.
"""
import base64
import ctypes
import hashlib
import hmac
import os
import struct
import sys
import time

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DLL_PATH = os.path.join(SCRIPT_DIR, 'lzfse-win64.dll')

lib_lzfse = ctypes.CDLL(DLL_PATH)
lib_lzfse.lzfse_decode_buffer.argtypes = [
    ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p
]
lib_lzfse.lzfse_decode_buffer.restype = ctypes.c_size_t

# Preallocate decompression buffer (1 MB segment size)
DECOMP_BUF_SIZE = 16 * 1024 * 1024
decomp_buf = ctypes.create_string_buffer(DECOMP_BUF_SIZE)


def derive_key(size, ikm, info, salt=b''):
    return HKDF(hashes.SHA256(), size, salt, info).derive(ikm)


def calculate_mac(key, data, salt):
    data = salt + data + struct.pack('<Q', len(salt))
    return hmac.digest(key, data, 'sha256')


def decrypt_and_verify(key, data, salt, mac):
    if mac != calculate_mac(key[:32], data, salt):
        raise ValueError('HMAC validation failed')
    cipher = Cipher(algorithms.AES(key[32:64]), modes.CTR(key[64:]))
    dec = cipher.decryptor()
    return dec.update(data)


def decompress_lzfse(compressed_data, original_size):
    global decomp_buf
    if original_size > len(decomp_buf):
        decomp_buf = ctypes.create_string_buffer(original_size + 65536)
    ret = lib_lzfse.lzfse_decode_buffer(
        decomp_buf, original_size, compressed_data, len(compressed_data), None
    )
    if ret != original_size:
        raise ValueError(f'LZFSE decompression failed: expected {original_size}, got {ret}')
    return decomp_buf.raw[:original_size]


def decrypt_aea_stream(in_stream, out_stream, symmetric_key_bytes, total_input_size=None):
    magic = in_stream.read(4)
    if magic != b'AEA1':
        raise ValueError(f'Not an AEA1 file: {magic}')

    profile, auth_size = struct.unpack('<II', in_stream.read(8))
    if profile != 1:
        raise ValueError(f'Unsupported profile: {profile} (expected 1)')

    auth_data = in_stream.read(auth_size)
    main_salt = in_stream.read(32)
    root_header_mac = in_stream.read(32)
    root_header_data = in_stream.read(48)
    cluster_mac = in_stream.read(32)

    # Profile 1 Key derivation
    stream_info = b'AEA_AMK' + struct.pack('<I', profile)[:3] + struct.pack('B', 0)
    main_key = derive_key(32, symmetric_key_bytes, stream_info, main_salt)
    root_header_key = derive_key(80, main_key, b'AEA_RHEK')

    rh_plain = decrypt_and_verify(root_header_key, root_header_data, cluster_mac + auth_data, root_header_mac)
    orig_sz, arch_sz, seg_sz, seg_per_cl = struct.unpack('<QQII', rh_plain[:24])
    comp_algo = chr(rh_plain[24])
    chk_algo = rh_plain[25]
    chk_sz = 32 if chk_algo == 2 else 0

    print(f'AEA Header Decrypted:')
    print(f'  Target Size: {orig_sz:,} bytes ({orig_sz / (1024**3):.2f} GiB)')
    print(f'  Archive Size: {arch_sz:,} bytes ({arch_sz / (1024**3):.2f} GiB)')
    print(f'  Segments: {seg_sz:,} bytes, {seg_per_cl} per cluster')
    print(f'  Compression: {comp_algo}, Checksum: {chk_algo}')

    seg_hdr_size = chk_sz + 8
    written = 0
    cluster_idx = 0
    t0 = time.time()
    last_print = t0

    while written < orig_sz:
        cluster_key = derive_key(32, main_key, b'AEA_CK' + struct.pack('<I', cluster_idx))
        cluster_header_key = derive_key(80, cluster_key, b'AEA_CHEK')

        seg_hdrs_raw = in_stream.read(seg_hdr_size * seg_per_cl)
        if len(seg_hdrs_raw) < seg_hdr_size * seg_per_cl:
            break
        next_cluster_mac = in_stream.read(32)
        segment_macs = in_stream.read(32 * seg_per_cl)

        segment_headers = decrypt_and_verify(
            cluster_header_key, seg_hdrs_raw,
            next_cluster_mac + segment_macs, cluster_mac
        )

        for i in range(seg_per_cl):
            seg_hdr = segment_headers[seg_hdr_size * i : seg_hdr_size * (i + 1)]
            orig_s, comp_s = struct.unpack_from('<II', seg_hdr)
            checksum = seg_hdr[8:]

            data_enc = in_stream.read(comp_s)
            mac_s = segment_macs[32 * i : 32 * (i + 1)]
            seg_key = derive_key(80, cluster_key, b'AEA_SK' + struct.pack('<I', i))
            data = decrypt_and_verify(seg_key, data_enc, b'', mac_s)

            if orig_s > comp_s:
                if comp_algo == 'e':
                    data = decompress_lzfse(data, orig_s)
                else:
                    raise ValueError(f'Unsupported compression: {comp_algo}')

            if len(data) != orig_s:
                raise ValueError(f'Size mismatch: expected {orig_s}, got {len(data)}')

            if chk_algo == 2:
                calc_chk = hashlib.sha256(data).digest()
                if calc_chk != checksum:
                    raise ValueError(f'Checksum mismatch on segment {i} of cluster {cluster_idx}')

            out_stream.write(data)
            written += len(data)

            if written >= orig_sz:
                break

        now = time.time()
        if now - last_print >= 2.0 or written >= orig_sz:
            pct = (written / orig_sz) * 100
            speed = (written / (1024 * 1024)) / (now - t0 + 0.001)
            print(f'  [{pct:5.1f}%] Decrypted {written / (1024**2):,.1f} MiB ({speed:.1f} MiB/s)', flush=True)
            last_print = now

        cluster_mac = next_cluster_mac
        cluster_idx += 1

    dt = time.time() - t0
    print(f'Done: {written:,} bytes in {dt:.1f}s ({written / (1024**2) / (dt + 0.001):.1f} MiB/s)')


def main():
    if len(sys.argv) < 4:
        print('Usage: aea_decrypt.py INPUT.aea OUTPUT.dmg KEY (base64:... or raw)')
        sys.exit(1)

    in_path = sys.argv[1]
    out_path = sys.argv[2]
    key_str = sys.argv[3]

    if key_str.startswith('base64:'):
        key_bytes = base64.b64decode(key_str[7:])
    else:
        key_bytes = base64.b64decode(key_str)

    print(f'Opening {in_path} -> {out_path}...')
    with open(in_path, 'rb') as fin, open(out_path, 'wb') as fout:
        decrypt_aea_stream(fin, fout, key_bytes)


if __name__ == '__main__':
    main()
