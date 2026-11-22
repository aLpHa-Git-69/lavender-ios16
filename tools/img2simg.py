#!/usr/bin/env python3
"""Convert a raw image to Android sparse v1 without losing zero-filled blocks.

Zero/repeated-word blocks use FILL, not DONT_CARE: flashing onto an existing
userdata partition must reproduce the image, including its zeros. Memory is
bounded; RAW chunks are split before the format's 32-bit size limit.
"""
import argparse
import os
from pathlib import Path
import struct
import tempfile

MAGIC, RAW, FILL = 0xED26FF3A, 0xCAC1, 0xCAC2
HEADER = struct.Struct("<IHHHHIIII")
CHUNK = struct.Struct("<HHII")
READ_SIZE = 8 << 20
UINT32_MAX = (1 << 32) - 1


def hardlink(source, target):
    """Python's Android builds omit os.link even when the filesystem supports it."""
    if hasattr(os, "link"):
        return os.link(source, target)
    import ctypes
    link = ctypes.CDLL(None, use_errno=True).link
    link.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    link.restype = ctypes.c_int
    if link(os.fsencode(source), os.fsencode(target)) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), os.fspath(target))


def convert(source, target, block=4096):
    source, target = Path(source), Path(target)
    if block <= 0 or block % 4 or block > READ_SIZE:
        raise ValueError("block size must be a positive multiple of 4, at most 8 MiB")
    if os.path.lexists(target):
        raise ValueError(f"refusing to overwrite {target}")
    size = source.stat().st_size
    if size == 0 or size % block:
        raise ValueError("raw image must be non-empty and a multiple of the block size")
    blocks = size // block
    if blocks > UINT32_MAX:
        raise ValueError("raw image exceeds the sparse format's block count")
    max_raw = (UINT32_MAX - CHUNK.size) // block
    runs = []  # (fill word or None for RAW, number of blocks)
    step = READ_SIZE // block * block
    with source.open("rb") as image:
        initial = os.fstat(image.fileno())
        scanned = 0
        while data := image.read(step):
            if len(data) % block:
                raise ValueError("raw image changed while scanning")
            for offset in range(0, len(data), block):
                piece = data[offset:offset + block]
                word = piece[:4]
                fill = word if piece == word * (block // 4) else None
                limit = UINT32_MAX if fill is not None else max_raw
                if runs and runs[-1][0] == fill and runs[-1][1] < limit:
                    runs[-1][1] += 1
                else:
                    runs.append([fill, 1])
                scanned += 1
        if scanned != blocks or os.fstat(image.fileno()).st_mtime_ns != initial.st_mtime_ns:
            raise ValueError("raw image changed while scanning")
        image.seek(0)
        temp = None
        try:
            with tempfile.NamedTemporaryFile(dir=target.parent, prefix=f".{target.name}.",
                                             delete=False) as output:
                temp = Path(output.name)
                output.write(HEADER.pack(MAGIC, 1, 0, HEADER.size, CHUNK.size,
                                         block, blocks, len(runs), 0))
                for fill, count in runs:
                    if fill is not None:
                        output.write(CHUNK.pack(FILL, 0, count, CHUNK.size + 4))
                        output.write(fill)
                        image.seek(count * block, os.SEEK_CUR)
                    else:
                        left = count * block
                        output.write(CHUNK.pack(RAW, 0, count, CHUNK.size + left))
                        while left:
                            data = image.read(min(left, READ_SIZE))
                            if not data:
                                raise ValueError("raw image changed while copying")
                            output.write(data)
                            left -= len(data)
                current = os.fstat(image.fileno())
                if (current.st_size, current.st_mtime_ns) != (initial.st_size, initial.st_mtime_ns):
                    raise ValueError("raw image changed while copying")
            hardlink(temp, target)
        finally:
            if temp is not None:
                temp.unlink(missing_ok=True)
    print(f"{target}: {blocks} blocks, {len(runs)} chunks, "
          f"{target.stat().st_size} bytes (raw {size} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("target")
    parser.add_argument("--block", type=int, default=4096)
    args = parser.parse_args()
    try:
        convert(args.source, args.target, args.block)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"img2simg: {exc}\n")


if __name__ == "__main__":
    main()
