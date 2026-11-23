#!/usr/bin/env python3
"""Verify boot-argument edits preserve the kernel, ADT, DTB and other components."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools/repack-shim-cmdline.py"
MAGIC = b"SWEETXNU"


def pack(data, page=4096):
    image = bytearray(page)
    image[:8] = b"ANDROID!"
    struct.pack_into("<10I", image, 8, len(data[0]), 0x8000, len(data[1]),
                     0x2000000, len(data[2]), 0xf00000, 0x100, page, 0, 0)
    image[48:64] = b"known-board".ljust(16, b"\0")
    image[64:80] = b"decoy arguments!"
    for part in data:
        image.extend(part)
        image.extend(b"\0" * (-len(part) % page))
    return bytes(image)


def unpack(data):
    header = struct.unpack_from("<10I", data, 8)
    page, pos = header[7], header[7]
    parts = []
    for size in (header[0], header[2], header[4]):
        parts.append(data[pos:pos + size])
        pos += (size + page - 1) // page * page
    assert pos == len(data)
    return data[:page], parts


def split_kernel(data):
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    plain = stream.decompress(data) + stream.flush()
    assert stream.eof
    return plain, stream.unused_data


def run(source, target, *args):
    return subprocess.run([sys.executable, str(TOOL), str(source), str(target), *args],
                          capture_output=True, text=True)


def main():
    # A decoy magic in shim code must not be confused with the appended payload.
    prefix = b"shim instructions " * 100 + MAGIC + b"padding" * 100
    fake = bytearray(4096)
    fake[:8] = MAGIC
    struct.pack_into("<II", fake, 8, 1, 3)
    line = b"rd=emmcroot0 rootrw=1 sweetouchi2c=0 sweetouchi2c=0 cpus=4"
    fake[0x48:0x48 + len(line)] = line
    fake[2048:] = bytes(range(256)) * 8
    plain = prefix + bytes(fake)
    compress = zlib.compressobj(9, zlib.DEFLATED, 31)
    dtb = b"opaque appended DTB\0" * 100
    parts = [compress.compress(plain) + compress.flush() + dtb,
             b"original ramdisk\0" * 37, b"original second stage\0" * 31]
    with tempfile.TemporaryDirectory() as td:
        source, target = Path(td) / "original.img", Path(td) / "candidate.img"
        original = pack(parts)
        source.write_bytes(original)
        result = run(source, target, "--set", "sweetouchi2c=1", "--set", "sweetouchwait=0")
        assert result.returncode == 0, result.stdout + result.stderr
        header, updated_parts = unpack(target.read_bytes())
        updated, updated_dtb = split_kernel(updated_parts[0])
        offset = len(prefix) + 0x48
        assert updated[:offset] == plain[:offset]
        assert updated[offset + 608:] == plain[offset + 608:], "kernel/ADT data changed"
        assert updated_dtb == dtb, "appended DTB changed"
        assert updated_parts[1:] == parts[1:], "ramdisk/second stage changed"
        new_line = updated[offset:offset + 608].split(b"\0", 1)[0].split()
        assert new_line.count(b"sweetouchi2c=1") == 1
        assert b"sweetouchi2c=0" not in new_line and b"sweetouchwait=0" in new_line
        assert b"rd=emmcroot0" in new_line and b"rootrw=1" in new_line and b"cpus=4" in new_line
        allowed = set(range(8, 12)) | set(range(576, 608))
        assert all(header[i] == original[i] for i in range(len(header)) if i not in allowed)
        expected = hashlib.sha1()
        for part in updated_parts:
            expected.update(part)
            expected.update(struct.pack("<I", len(part)))
        assert header[576:608] == expected.digest().ljust(32, b"\0")
        assert source.read_bytes() == original, "original image was altered"
        before = target.read_bytes()
        assert run(source, target, "--set", "sweetouchwait=0").returncode != 0
        assert target.read_bytes() == before
        assert run(source, source, "--set", "sweetouchwait=0").returncode != 0
        for setting in ("invalid", "a=two words", "a=" + "x" * 608):
            bad = Path(td) / "bad.img"
            assert run(source, bad, "--set", setting).returncode != 0
            assert not bad.exists()
        unsupported = bytearray(original)
        struct.pack_into("<I", unsupported, 40, 2)
        source.write_bytes(unsupported)
        assert run(source, Path(td) / "v2.img", "--set", "a=1").returncode != 0
        assert not (Path(td) / "v2.img").exists()
    print("test_repack_shim: payload-only edit, preserved components, header ID and rejection checks passed")


if __name__ == "__main__":
    main()
