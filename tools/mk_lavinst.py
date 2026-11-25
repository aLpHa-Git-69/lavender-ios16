#!/usr/bin/env python3
"""Pack a LAVINST1 archive for XNU's lavinstall=1 RAMDisk installer.

Usage:
  mk_lavinst.py OUT DIR_SPEC... FILE_SPEC...
    DIR_SPEC:  dir:/path:0755
    FILE_SPEC: file:/path:0644:/local/host/file
"""
import hashlib
import pathlib
import struct
import sys

PATH_FIELD = 256


def pack_archive(specs):
    buf = bytearray(b"LAVINST1")
    ndirs = nfiles = 0
    for spec in specs:
        parts = spec.split(":", 3)
        kind_s = parts[0]
        if kind_s == "dir":
            _, dst, mode_s = parts
            mode = int(mode_s, 8)
            raw_path = dst.encode("ascii")
            assert len(raw_path) < PATH_FIELD, f"path too long: {dst}"
            buf += struct.pack("<IIQ", 1, mode, 0)
            buf += raw_path.ljust(PATH_FIELD, b"\0")
            ndirs += 1
        elif kind_s == "file":
            _, dst, mode_s, src = parts
            mode = int(mode_s, 8)
            raw_path = dst.encode("ascii")
            assert len(raw_path) < PATH_FIELD, f"path too long: {dst}"
            data = pathlib.Path(src).read_bytes()
            buf += struct.pack("<IIQ", 2, mode, len(data))
            buf += raw_path.ljust(PATH_FIELD, b"\0")
            buf += data
            buf += b"\0" * (-len(data) & 15)
            nfiles += 1
        else:
            raise ValueError(f"unknown spec: {spec}")
    buf += struct.pack("<IIQ", 0, 0, 0)
    return bytes(buf), ndirs, nfiles


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[1])
    blob, ndirs, nfiles = pack_archive(sys.argv[2:])
    out.write_bytes(blob)
    print(f"{out}: {len(blob)} bytes ({ndirs} dirs, {nfiles} files, sha256 {hashlib.sha256(blob).hexdigest()[:16]}...)")


if __name__ == "__main__":
    main()
