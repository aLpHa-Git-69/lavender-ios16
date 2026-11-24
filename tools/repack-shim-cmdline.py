#!/usr/bin/env python3
"""Change SWEETXNU boot arguments in an existing Android v0 shim image.

  python3 tools/repack-shim-cmdline.py ORIGINAL.img CANDIDATE.img \
    --set sweetouchi2c=1 --set sweetouchwait=0

In command-line mode the decompressed image changes only in the payload's
608-byte argument field. Its appended DTB, ramdisk and second stage are preserved, and the
Android header sizes and SHA-1 component ID are recomputed. Output must be
a new file. Other boot-header versions are deliberately rejected.

--kernel KERNEL.development.lavender replaces the kernel payload using the
image's existing board ADT and shim instructions. This mode is for packaging
a rebuilt kernel against an already-booting lavender image.
--ramdisk STATIC_INIT.macho replaces the embedded payload RAMDisk for a
diagnostic boot; it requires --kernel. Set rd=md0 to boot that RAMDisk.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import tempfile
import zlib

import adt
import check_payload
import mkpayload
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


def kernel_payload(original, start, line, kernel, replacement_disk=None):
    check_payload.check_kernel(kernel, "lavender")
    old = original[start:]
    offset, size = struct.unpack_from("<QQ", old, 0x20)
    if size == 0 or offset + size > len(old):
        raise ValueError("original board ADT is truncated")
    board_adt = old[offset:offset + size]
    root, consumed = adt.parse(board_adt)
    if consumed != len(board_adt):
        raise ValueError("original board ADT has unparsed data")
    chosen = next((c for c in root.children if c.name == "chosen"), None)
    if chosen is None:
        raise ValueError("original ADT has no chosen node")
    memory_map = next((c for c in chosen.children if c.name == "memory-map"), None)
    disk_property = memory_map.prop("RAMDisk") if memory_map is not None else None
    disk_data, disk_index, disk_flags = None, None, None
    if disk_property is not None:
        disk_data = payload_ramdisk(old, disk_property)
        disk_index = next(i for i, p in enumerate(memory_map.props) if p[0] == "RAMDisk")
        disk_flags = memory_map.props[disk_index][1]
        del memory_map.props[disk_index]
    if replacement_disk is not None:
        if disk_property is None:
            raise ValueError("RAMDisk replacement requires an existing payload RAMDisk")
        disk_data = Path(replacement_disk).read_bytes()
        if not disk_data:
            raise ValueError("replacement RAMDisk is empty")
    with tempfile.TemporaryDirectory(prefix="shim-adt-") as directory:
        path = Path(directory) / "board.adt"
        path.write_bytes(adt.serialize(root) if disk_data is not None else board_adt)
        disk_path = None
        if disk_data is not None:
            disk_path = Path(directory) / "ramdisk.img"
            disk_path.write_bytes(disk_data)
        try:
            packed, info = mkpayload.build(str(kernel), str(path), line.decode("ascii"),
                                           str(disk_path) if disk_path is not None else None)
        except AssertionError as exc:
            raise ValueError(f"invalid kernel payload: {exc}") from exc
    new_offset, new_size = struct.unpack_from("<QQ", packed, 0x20)
    if disk_data is None:
        if packed[new_offset:new_offset + new_size] != board_adt:
            raise ValueError("kernel replacement changed the board ADT")
    else:
        new_root, _ = adt.parse(packed[new_offset:new_offset + new_size])
        new_chosen = next(c for c in new_root.children if c.name == "chosen")
        new_map = next(c for c in new_chosen.children if c.name == "memory-map")
        new_disk = new_map.prop("RAMDisk")
        if new_disk is None or payload_ramdisk(packed, new_disk) != disk_data:
            raise ValueError("kernel replacement changed the embedded RAMDisk data")
        # Keep the original property position/flags while updating its new PA.
        new_map.props = [p for p in new_map.props if p[0] != "RAMDisk"]
        new_map.props.insert(disk_index, ("RAMDisk", disk_flags, new_disk))
        relocated_adt = adt.serialize(new_root)
        if len(relocated_adt) != len(board_adt):
            raise ValueError("RAMDisk relocation changed the board ADT size")
        new_map.props[disk_index] = ("RAMDisk", disk_flags, disk_property)
        if adt.serialize(new_root) != board_adt:
            raise ValueError("kernel replacement changed board properties outside RAMDisk")
        packed = bytearray(packed)
        packed[new_offset:new_offset + new_size] = relocated_adt
    print(f"replacement kernel: {kernel}, {info['size']} payload bytes, {info['fixups']} fixups")
    return original[:start] + packed


def payload_ramdisk(packed, prop):
    if len(prop) != 16:
        raise ValueError("invalid embedded RAMDisk property")
    address, size = struct.unpack("<QQ", prop)
    virtual_base = struct.unpack_from("<Q", packed, 0x10)[0]
    physical_base = struct.unpack_from("<Q", packed, 0x38)[0]
    count = struct.unpack_from("<I", packed, 12)[0]
    if size == 0 or count == 0 or count > 32:
        raise ValueError("invalid embedded RAMDisk segment table")
    matches = []
    for index in range(count):
        va, span, offset, length = struct.unpack_from("<4Q", packed, 0x2a8 + index * 32)
        if va - virtual_base + physical_base == address and span == size:
            if length == 0 or length > size or offset + length > len(packed):
                raise ValueError("embedded RAMDisk segment is truncated")
            matches.append(bytes(packed[offset:offset + length]))
    if len(matches) != 1:
        raise ValueError("embedded RAMDisk must match exactly one payload segment")
    return matches[0]


def repack(source, target, settings, kernel=None, ramdisk=None):
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
    if kernel is not None:
        updated = kernel_payload(original, start, line, kernel, ramdisk)
    else:
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
    parser.add_argument("--set", dest="settings", action="append", default=[],
                        metavar="KEY=VALUE", help="replace all existing instances, or append")
    parser.add_argument("--kernel", help="rebuild the payload from this raw arm64 Mach-O kernel")
    parser.add_argument("--ramdisk", help="replace the embedded payload RAMDisk (requires --kernel)")
    args = parser.parse_args()
    if not args.settings and not args.kernel:
        parser.error("provide --set or --kernel")
    if args.ramdisk and not args.kernel:
        parser.error("--ramdisk requires --kernel")
    try:
        repack(args.source, args.target, args.settings, args.kernel, args.ramdisk)
    except (OSError, ValueError, zlib.error) as exc:
        parser.exit(1, f"repack-shim: {exc}\n")


if __name__ == "__main__":
    main()
