#!/usr/bin/env python3
"""Extract the raw PE image ("basefile") from a retail XEX2.

Usage: xex_extract.py <default.xex> <out.pe>

Supports encryption type 1 (retail AES key) and compression types 0 (none)
and 1 (basic / zero-block). LZX ("normal") compression is not handled.
"""
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def aes(key, data, mode):
    d = Cipher(algorithms.AES(key), mode).decryptor()
    return d.update(data) + d.finalize()


def main(src, dst):
    d = open(src, "rb").read()
    magic, _flags, pe_off, _res, sec_off, nh = struct.unpack(">4sIIIII", d[:24])
    if magic != b"XEX2":
        raise SystemExit("not an XEX2 file")
    headers = dict(struct.unpack_from(">II", d, 24 + i * 8) for i in range(nh))

    ffi = headers[0x3FF]
    info_size, enc, comp = struct.unpack_from(">IHH", d, ffi)
    payload = d[pe_off:]

    if enc == 1:
        file_key = d[sec_off + 0x150 : sec_off + 0x160]
        session = aes(RETAIL_KEY, file_key, modes.ECB())
        payload = aes(session, payload[: len(payload) & ~15], modes.CBC(bytes(16)))

    if comp == 0:
        image = payload
    elif comp == 1:
        out = bytearray()
        pos = 0
        for i in range((info_size - 8) // 8):
            data_size, zero_size = struct.unpack_from(">II", d, ffi + 8 + i * 8)
            out += payload[pos : pos + data_size]
            out += bytes(zero_size)
            pos += data_size
        image = bytes(out)
    else:
        raise SystemExit(f"compression type {comp} (LZX) not supported")

    if image[:2] != b"MZ":
        raise SystemExit("decryption failed: no MZ header")
    open(dst, "wb").write(image)
    print(f"wrote {len(image)} bytes, load base 0x{headers.get(0x10201, 0):08X}, "
          f"entry 0x{headers.get(0x10100, 0):08X}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], sys.argv[2])
