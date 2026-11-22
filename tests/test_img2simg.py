#!/usr/bin/env python3
"""Check sparse output using an independent decoder and a dirty target device."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

TOOL = Path(__file__).resolve().parent.parent / "tools/img2simg.py"


def expand(blob):
    magic, major, minor, header_size, chunk_size, block, count, chunks, _crc = \
        struct.unpack_from("<IHHHHIIII", blob)
    assert (magic, major, minor, header_size, chunk_size) == (0xED26FF3A, 1, 0, 28, 12)
    device = bytearray(b"\xa5" * (count * block))
    offset, position = header_size, 0
    kinds = []
    for _ in range(chunks):
        kind, reserved, length, total = struct.unpack_from("<HHII", blob, offset)
        assert reserved == 0 and length > 0
        payload = blob[offset + chunk_size:offset + total]
        if kind == 0xCAC1:
            assert len(payload) == length * block
            device[position:position + len(payload)] = payload
        elif kind == 0xCAC2:
            assert len(payload) == 4
            device[position:position + length * block] = payload * (length * block // 4)
        else:
            raise AssertionError(f"unexpected chunk {kind:#x}; DONT_CARE would leave stale bytes")
        position += length * block
        offset += total
        kinds.append(kind)
    assert position == len(device) and offset == len(blob)
    return bytes(device), kinds


def run(source, target, *args):
    return subprocess.run([sys.executable, str(TOOL), str(source), str(target), *args],
                          capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory() as td:
        source, target = Path(td) / "root.img", Path(td) / "root.simg"
        raw = (bytes(8192) + bytes(range(256)) * 32 + b"\x12\x34\x56\x78" * 2048
               + bytes(range(255, -1, -1)) * 16 + bytes(4096))
        source.write_bytes(raw)
        result = run(source, target)
        assert result.returncode == 0, result.stdout + result.stderr
        decoded, kinds = expand(target.read_bytes())
        assert decoded == raw, "sparse flash did not reproduce zeros on a dirty device"
        assert set(kinds) == {0xCAC1, 0xCAC2}, kinds

        original = target.read_bytes()
        result = run(source, target)
        assert result.returncode != 0 and target.read_bytes() == original
        result = run(source, source)
        assert result.returncode != 0 and source.read_bytes() == raw
        for block in ("0", "-4", "3", str(16 << 20)):
            dest = Path(td) / f"bad-{block}.simg"
            assert run(source, dest, "--block", block).returncode != 0
            assert not dest.exists()

        source.write_bytes(b"not block aligned")
        dest = Path(td) / "short.simg"
        assert run(source, dest).returncode != 0 and not dest.exists()
        source.write_bytes(b"")
        assert run(source, dest).returncode != 0 and not dest.exists()

        # A long zero run is encoded with one FILL chunk, not materialized data.
        with source.open("wb") as image:
            image.truncate(32 << 20)
        dest = Path(td) / "zeros.simg"
        result = run(source, dest)
        assert result.returncode == 0, result.stderr
        assert dest.stat().st_size == 44
        assert expand(dest.read_bytes())[0] == bytes(32 << 20)
        assert not list(Path(td).glob(".root.simg.*")), "temporary output leaked"
    print("test_img2simg: exact RAW/FILL round-trip, dirty target, alignment and overwrite checks passed")


if __name__ == "__main__":
    main()
