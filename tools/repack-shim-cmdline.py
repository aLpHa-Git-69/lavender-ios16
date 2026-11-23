#!/usr/bin/env python3
"""Change SWEETXNU boot arguments in an existing Android v0 shim image.

  python3 tools/repack-shim-cmdline.py ORIGINAL.img CANDIDATE.img \
    --set sweetouchi2c=1 --set sweetouchwait=0

The decompressed image changes only in the payload's 608-byte command-line
field. Its appended DTB, ramdisk and second stage are preserved, and the
Android header sizes and SHA-1 component ID are recomputed. Output must be
a new file. Other boot-header versions are deliberately rejected.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import zlib

import payload_cmdline as payload


def components(data):
    if len(data) < 1632 or data[:8] != b"ANDROID!":
        raise ValueError("not an Android boot image")
    values = struct.unpack_from("<10I", data, 8)
    page, version = values[7:9]
    if version != 0:
        raise ValueError("this repacker supports Android boot-header v0 only")
    if page < 2048 or page > 65536 or page & (page - 1):
        raise ValueError("invalid boot-image page size")
    if len(data) < page or values[0] == 0:
        raise ValueError("truncated or empty boot image")
    cursor = page
    parts = []
    for size in (values[0], values[2], values[4]):
        end = cursor + size
        padded = cursor + (size + page - 1) // page * page
        if padded > len(data):
            raise ValueError("boot-image component is truncated")
        parts.append(data[cursor:end])
        cursor = padded
    trailer = data[cursor:]
    if any(trailer):
        raise ValueError("unrecognized non-zero boot-image trailer")
    return page, parts, trailer


def repack(source, target, settings):
    if os.path.lexists(target):
        raise ValueError(f"refusing to overwrite {target}")
    data = Path(source).read_bytes()
    page, parts, trailer = components(data)
    unzip = zlib.decompressobj(16 + zlib.MAX_WBITS)
    original = unzip.decompress(parts[0]) + unzip.flush()
    if not unzip.eof:
        raise ValueError("truncated gzip shim")
    suffix = unzip.unused_data
    start = payload.payload_offset(original)
    if struct.unpack_from("<I", original, start + 8)[0] != 1:
        raise ValueError("unsupported SWEETXNU payload version")
    begin = start + payload.CMDLINE_OFF
    end = begin + payload.CMDLINE_LEN
    field = original[begin:end]
    if b"\0" not in field:
        raise ValueError("original payload command line is not terminated")
    words = field.split(b"\0", 1)[0].decode("ascii").split()
    for setting in settings:
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]*=[^\s\x00]+", setting, re.ASCII):
            raise ValueError(f"expected a single KEY=VALUE token: {setting!r}")
        key = setting.split("=", 1)[0]
        words = [word for word in words if word != key and not word.startswith(key + "=")]
        words.append(setting)
    line = " ".join(words).encode("ascii")
    if len(line) >= payload.CMDLINE_LEN:
        raise ValueError("command line exceeds the payload's 607-byte limit")
    updated = bytearray(original)
    updated[begin:end] = line.ljust(payload.CMDLINE_LEN, b"\0")
    assert updated[:begin] == original[:begin] and updated[end:] == original[end:]
    compressor = zlib.compressobj(9, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
    parts[0] = compressor.compress(updated) + compressor.flush() + suffix
    header = bytearray(data[:page])
    struct.pack_into("<I", header, 8, len(parts[0]))
    digest = hashlib.sha1()
    for part in parts:
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    header[576:608] = digest.digest().ljust(32, b"\0")
    output = bytearray(header)
    for part in parts:
        output.extend(part)
        output.extend(b"\0" * (-len(part) % page))
    output.extend(trailer)
    with open(target, "xb") as stream:
        stream.write(output)
    print(f"payload command line: {line.decode('ascii')}")
    print(f"candidate: {target} ({len(output)} bytes)")
    print(f"sha256: {hashlib.sha256(output).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("target")
    parser.add_argument("--set", dest="settings", action="append", required=True,
                        metavar="KEY=VALUE", help="replace all existing instances, or append")
    args = parser.parse_args()
    try:
        repack(args.source, args.target, args.settings)
    except (OSError, ValueError, zlib.error) as exc:
        parser.exit(1, f"repack-shim: {exc}\n")


if __name__ == "__main__":
    main()
