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
sys.path.insert(0, str(ROOT / "tools"))
import adt
import mkpayload


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


def synthetic_kernel(marker):
    data = bytearray(4096)
    base = 0xfffffff007004000
    struct.pack_into("<8I", data, 0, 0xfeedfacf, 0x0100000c, 0, 2, 2, 72 + 288, 0x200000, 0)
    struct.pack_into("<II16s4Q4I", data, 32, 0x19, 72, b"__TEXT".ljust(16, b"\0"),
                     base, 0x4000, 0, len(data), 7, 5, 0, 0)
    struct.pack_into("<4I", data, 104, 5, 288, 6, 68)
    struct.pack_into("<Q", data, 104 + 16 + 32 * 8, base + 1024)
    data[1024:1024 + len(marker)] = marker
    return bytes(data)


def test_kernel_replacement(td, prefix, dtb, ramdisk, second, embedded=False, replace_disk=False):
    root, chosen = adt.Node(), adt.Node()
    root.props = [("name", 0, b"device-tree\0")]
    chosen.props = [("name", 0, b"chosen\0"),
                    ("dram-base", 0, struct.pack("<Q", 0xa0000000)),
                    ("dram-size", 0, struct.pack("<Q", 0x40000000))]
    root.children = [chosen]
    memory_map = adt.Node()
    memory_map.props = [("name", 0, b"memory-map\0")]
    chosen.children = [memory_map]
    blob = adt.serialize(root)
    adt_path, old_kernel, new_kernel = td / "board.adt", td / "old.kernel", td / "new.kernel"
    adt_path.write_bytes(blob)
    old_kernel.write_bytes(synthetic_kernel(b"original code"))
    replacement = synthetic_kernel(b"fixed Novatek code")
    new_kernel.write_bytes(replacement)
    payload_disk = None
    if embedded:
        payload_disk = td / "payload-ramdisk.img"
        payload_disk.write_bytes(b"preserved embedded PID 1 ramdisk\0" * 100)
    old_payload, _ = mkpayload.build(str(old_kernel), str(adt_path), "rd=emmcroot0 cpus=4",
                                    str(payload_disk) if payload_disk else None)
    zip_old = zlib.compressobj(9, zlib.DEFLATED, 31)
    old_parts = [zip_old.compress(prefix + old_payload) + zip_old.flush() + dtb, ramdisk, second]
    original = pack(old_parts)
    source, target = td / "kernel-source.img", td / "kernel-candidate.img"
    source.write_bytes(original)
    arguments = ["--kernel", str(new_kernel), "--set", "sweetouchi2c=1"]
    expected_disk = payload_disk.read_bytes() if payload_disk else None
    if replace_disk:
        new_disk = td / "new-ramdisk.img"
        expected_disk = b"replacement read-only rescue PID 1\0" * 700
        new_disk.write_bytes(expected_disk)
        arguments.extend(["--ramdisk", str(new_disk)])
    result = run(source, target, *arguments)
    assert result.returncode == 0, result.stdout + result.stderr
    header, parts = unpack(target.read_bytes())
    image, tail = split_kernel(parts[0])
    assert image[:len(prefix)] == prefix and tail == dtb
    assert parts[1:] == [ramdisk, second]
    payload = image[len(prefix):]
    ao, size = struct.unpack_from("<QQ", payload, 0x20)
    actual_adt = payload[ao:ao + size]
    if embedded:
        node, _ = adt.parse(actual_adt)
        mmap = next(c for c in next(c for c in node.children if c.name == "chosen").children
                    if c.name == "memory-map")
        disk_pa, disk_size = struct.unpack("<QQ", mmap.prop("RAMDisk"))
        virt, phys = struct.unpack_from("<Q", payload, 0x10)[0], struct.unpack_from("<Q", payload, 0x38)[0]
        segments = [struct.unpack_from("<4Q", payload, 0x2a8 + i * 32)
                    for i in range(struct.unpack_from("<I", payload, 12)[0])]
        disk = next(s for s in segments if s[0] - virt + phys == disk_pa and s[1] == disk_size)
        assert payload[disk[2]:disk[2] + disk[3]] == expected_disk
        mmap.props = [p for p in mmap.props if p[0] != "RAMDisk"]
        assert adt.serialize(node) == blob, "board properties outside RAMDisk changed"
    else:
        assert actual_adt == blob, "replacement lost the original board ADT"
    _, _, offset, count = struct.unpack_from("<4Q", payload, 0x2a8)
    assert payload[offset:offset + count] == replacement, "replacement kernel data differs"
    assert struct.unpack_from("<QQ", payload, 0x38) == (0xa0000000, 0x40000000)
    assert b"sweetouchi2c=1" in payload[0x48:0x2a8].split(b"\0", 1)[0].split()
    expected = hashlib.sha1()
    for part in parts:
        expected.update(part); expected.update(struct.pack("<I", len(part)))
    assert header[576:608] == expected.digest().ljust(32, b"\0")
    assert source.read_bytes() == original
    invalid = td / "not-a-kernel"
    invalid.write_bytes(b"invalid")
    rejected = td / "invalid-kernel.img"
    assert run(source, rejected, "--kernel", str(invalid)).returncode != 0
    assert not rejected.exists()


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
        test_kernel_replacement(Path(td), prefix, dtb, parts[1], parts[2])
        embedded_dir = Path(td) / "embedded"
        embedded_dir.mkdir()
        test_kernel_replacement(embedded_dir, prefix, dtb, parts[1], parts[2], embedded=True)
        rescue_dir = Path(td) / "rescue"
        rescue_dir.mkdir()
        test_kernel_replacement(rescue_dir, prefix, dtb, parts[1], parts[2],
                                embedded=True, replace_disk=True)
    print("test_repack_shim: command-line edits, kernel replacement, preserved board data and header ID passed")


if __name__ == "__main__":
    main()
