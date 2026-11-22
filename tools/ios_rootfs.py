#!/usr/bin/env python3
"""Download, extract, prepare and check the lavender iOS 16 root filesystem.

Download/extract work in Termux. Preparing a mounted image requires root and
a host that can mount APFS and case-sensitive HFS+; see docs/ROOTFS.md.
"""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import stat
import subprocess
import sys
import zipfile
from img2simg import hardlink

HERE = Path(__file__).resolve().parent
VERSION, BUILD, PRODUCT, BOARD = "16.7.16", "20H392", "iPhone10,3", "d22ap"
IPSW_URL = (
    "https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/"
    "1158E502-D76D-4226-AA06-ED5E30CD1111/"
    "iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw"
)
IPSW_SHA256 = "ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb"
COMPONENTS = {"OS": "OS.dmg", "Cryptex1,SystemOS": "SystemOS.dmg",
              "Cryptex1,AppOS": "AppOS.dmg"}
CACHE = "System/Library/Caches/com.apple.dyld"
PREFS = "private/var/mobile/Library/Preferences"


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for data in iter(lambda: stream.read(8 << 20), b""):
            digest.update(data)
    return digest.hexdigest()


def verify_archive(path, expected):
    actual = sha256(path)
    if actual != expected.lower():
        raise ValueError(f"{path}: SHA-256 {actual}, expected {expected}")
    return actual


def download(path):
    path = Path(path)
    if path.exists():
        verify_archive(path, IPSW_SHA256)
        print(f"already verified: {path}")
        return
    part = path.with_name(path.name + ".part")
    if path.is_symlink() or part.is_symlink():
        raise ValueError("download output must not be a symlink")
    print(f"downloading {PRODUCT} iOS {VERSION} ({BUILD}) from Apple", flush=True)
    subprocess.run([
        "curl", "--fail", "--location", "--silent", "--show-error",
        "--proto", "=https", "--proto-redir", "=https", "--retry", "3",
        "--connect-timeout", "30", "--max-time", "1800", "--continue-at", "-",
        "--output", str(part), IPSW_URL,
    ], check=True)
    verify_archive(part, IPSW_SHA256)
    hardlink(part, path)  # Publish without replacing an existing output.
    part.unlink()
    print(f"verified IPSW: {path} sha256 {IPSW_SHA256}")


def manifest_paths(manifest):
    if (manifest.get("ProductVersion"), manifest.get("ProductBuildVersion")) != (VERSION, BUILD):
        raise ValueError(f"root tweaks require iOS {VERSION} ({BUILD})")
    if PRODUCT not in manifest.get("SupportedProductTypes", []):
        raise ValueError(f"IPSW does not support {PRODUCT}")
    identities = [identity for identity in manifest.get("BuildIdentities", [])
                  if identity.get("Info", {}).get("DeviceClass", "").lower() == BOARD
                  and identity.get("Info", {}).get("RestoreBehavior") == "Erase"]
    if len(identities) != 1:
        raise ValueError(f"expected one {BOARD} erase identity, found {len(identities)}")
    paths = {}
    for component, output in COMPONENTS.items():
        name = identities[0].get("Manifest", {}).get(component, {}).get("Info", {}).get("Path")
        if not isinstance(name, str) or not name.endswith(".dmg"):
            raise ValueError(f"missing DMG path for {component}")
        parts = PurePosixPath(name)
        if parts.is_absolute() or ".." in parts.parts or "\\" in name:
            raise ValueError(f"invalid IPSW member path: {name}")
        paths[output] = name
    return paths


def extract(archive, out, expected=IPSW_SHA256):
    out = Path(out)
    if os.path.lexists(out):
        raise ValueError(f"refusing to overwrite {out}")
    digest = verify_archive(archive, expected)
    with zipfile.ZipFile(archive) as package:
        names = package.namelist()
        if names.count("BuildManifest.plist") != 1:
            raise ValueError("IPSW must contain one BuildManifest.plist")
        if package.getinfo("BuildManifest.plist").file_size > 8 << 20:
            raise ValueError("BuildManifest.plist is unexpectedly large")
        paths = manifest_paths(plistlib.loads(package.read("BuildManifest.plist")))
        for name in paths.values():
            if names.count(name) != 1 or package.getinfo(name).file_size == 0:
                raise ValueError(f"missing, duplicate or empty DMG: {name}")
        out.mkdir()
        metadata = {"product": PRODUCT, "board": BOARD, "version": VERSION,
                    "build": BUILD, "ipsw_sha256": digest, "components": {}}
        for output, name in paths.items():
            dest = out / output  # Fixed output names, never archive-controlled paths.
            print(f"extracting {name} -> {dest}", flush=True)
            try:
                with package.open(name) as source, dest.open("xb") as target:
                    shutil.copyfileobj(source, target, 8 << 20)
            except BaseException:
                dest.unlink(missing_ok=True)
                raise
            metadata["components"][output] = {
                "member": name, "bytes": dest.stat().st_size, "sha256": sha256(dest)}
        (out / "rootfs-source.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"extracted and CRC-checked all three DMGs into {out}")


def image_path(root, relative, follow_final=True):
    """Resolve iOS symlinks inside the image, including absolute /private links.

    Host Path.resolve() would follow an absolute iOS link into the host's own
    filesystem. Walk links explicitly with the image as '/' instead.
    """
    root = Path(root).resolve()
    pending, resolved, links = deque(PurePosixPath(relative).parts), [], 0
    while pending:
        part = pending.popleft()
        if part == "/":
            resolved.clear()
            continue
        if part in ("", "."):
            continue
        if part == "..":
            if resolved:
                resolved.pop()
            continue
        candidate = root.joinpath(*resolved, part)
        if candidate.is_symlink() and (pending or follow_final):
            links += 1
            if links > 40:
                raise ValueError(f"symlink loop in image: {relative}")
            target = PurePosixPath(os.readlink(candidate))
            pending.extendleft(reversed(target.parts))
        else:
            resolved.append(part)
    return root.joinpath(*resolved)


def load_plist(root, relative):
    with image_path(root, relative).open("rb") as stream:
        return plistlib.load(stream)


def check_version(root):
    version = load_plist(root, "System/Library/CoreServices/SystemVersion.plist")
    if (version.get("ProductVersion"), version.get("ProductBuildVersion")) != (VERSION, BUILD):
        raise ValueError(f"mounted root must be iOS {VERSION} ({BUILD})")


def owned_dir(path, uid, gid, mode):
    path.mkdir(parents=True, exist_ok=True)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def stage(root):
    """Complete the single-volume layout after copying OS and both cryptexes."""
    check_version(root)
    source_cache = image_path(root, f"private/preboot/Cryptexes/OS/{CACHE}")
    if not (source_cache / "dyld_shared_cache_arm64").is_file():
        raise ValueError("SystemOS cryptex has no arm64 dyld shared cache")
    target_cache = image_path(root, CACHE)
    target_cache.mkdir(parents=True, exist_ok=True)
    for source in sorted(source_cache.iterdir()):
        if not source.is_file() or source.is_symlink():
            continue
        target = target_cache / source.name
        if os.path.lexists(target):
            if not os.path.samefile(source, target):
                raise ValueError(f"root dyld cache differs from cryptex: {source.name}")
        else:
            hardlink(source, target)

    linked = 0
    for cryptex in ("OS", "App"):
        source_root = image_path(root, f"private/preboot/Cryptexes/{cryptex}")
        if not source_root.is_dir():
            raise ValueError(f"missing {cryptex} cryptex")
        for directory, dirs, files in os.walk(source_root, followlinks=False):
            directory = Path(directory)
            relative = directory.relative_to(source_root)
            if relative == Path(CACHE):
                dirs.clear()
                continue
            dest_dir = image_path(root, relative.as_posix())
            if not dest_dir.exists():
                dest_dir.mkdir(parents=True)
                shutil.copystat(directory, dest_dir)
                st = directory.stat()
                os.chown(dest_dir, st.st_uid, st.st_gid)
            entries = files + [name for name in dirs if (directory / name).is_symlink()]
            for name in entries:
                source = directory / name
                target = image_path(root, (relative / name).as_posix(), follow_final=False)
                if os.path.lexists(target):
                    continue
                if source.is_symlink():
                    os.symlink(os.readlink(source), target)
                    st = source.lstat()
                    os.lchown(target, st.st_uid, st.st_gid)
                elif source.is_file():
                    hardlink(source, target)
                else:
                    raise ValueError(f"unsupported cryptex entry: {source}")
                linked += 1
    print(f"linked {linked} additional cryptex entries into the root")

    db = "private/var/db"
    for name, mode in (("diagnostics", 0o751), ("uuidtext", 0o755),
                       ("diagnostics/timesync", 0o755)):
        directory = image_path(root, f"{db}/{name}")
        owned_dir(directory, 272, 272, mode)
        marker = image_path(root, f"{db}/{name}/.migrated")
        marker.write_bytes(b"")
        os.chown(marker, 272, 272)
        os.chmod(marker, 0o644)
    owned_dir(image_path(root, "private/var/keybags"), 0, 0, 0o700)
    keybag = image_path(root, "private/var/keybags/systembag.kb")
    if not keybag.exists():
        with keybag.open("xb") as stream:
            plistlib.dump({"KeyBagKeys": b"SWEET"}, stream)
        os.chown(keybag, 0, 0)
        os.chmod(keybag, 0o600)


def check(root, ownership=True):
    check_version(root)
    for relative in ("sbin/launchd", "usr/lib/dyld"):
        path = image_path(root, relative)
        if not path.is_file() or not path.stat().st_mode & 0o111:
            raise ValueError(f"missing executable: /{relative}")
        if ownership and path.stat().st_uid != 0:
            raise ValueError(f"/{relative} must be owned by root")
    source_cache = image_path(root, f"private/preboot/Cryptexes/OS/{CACHE}")
    target_cache = image_path(root, CACHE)
    if not (source_cache / "dyld_shared_cache_arm64").is_file():
        raise ValueError("missing arm64 dyld shared cache")
    for source in source_cache.glob("dyld_shared_cache_arm64*"):
        if not os.path.samefile(source, target_cache / source.name):
            raise ValueError(f"dyld cache is not hard-linked: {source.name}")
    jobs = load_plist(root, "System/Library/xpc/launchd.plist")["LaunchDaemons"]
    if any("_PanicOnCrash" in job or job.get("Label") == "com.apple.CommCenter"
           for job in jobs.values()):
        raise ValueError("launchd bring-up tweaks have not been applied")
    for name in ("com.apple.migration", "com.apple.DataMigration"):
        relative = f"{PREFS}/{name}.plist"
        if load_plist(root, relative).get("LastSystemVersion") != BUILD:
            raise ValueError(f"missing {BUILD} migration marker: {name}")
        st = image_path(root, relative).stat()
        if ownership and (st.st_uid, st.st_gid, stat.S_IMODE(st.st_mode)) != (501, 501, 0o600):
            raise ValueError(f"{name}.plist must be 501:501, mode 0600")
    if not load_plist(root, f"{PREFS}/com.apple.springboard.plist").get("BKNoWatchdogs"):
        raise ValueError("SpringBoard bring-up watchdog preference is missing")
    if not load_plist(root, f"{PREFS}/com.apple.purplebuddy.plist").get("SetupDone"):
        raise ValueError("Setup preference is missing")
    for name in ("Bundle", "Data/System", "Shared/SystemGroup", "Temp"):
        if not image_path(root, f"private/var/containers/{name}").is_dir():
            raise ValueError(f"missing container directory: {name}")
    for name in ("diagnostics", "uuidtext", "diagnostics/timesync"):
        marker = image_path(root, f"private/var/db/{name}/.migrated")
        if not marker.is_file() or (ownership and marker.stat().st_uid != 272):
            raise ValueError(f"logd migration marker missing or incorrectly owned: {name}")
    if not isinstance(load_plist(root, "private/var/keybags/systembag.kb").get("KeyBagKeys"), bytes):
        raise ValueError("system keybag is missing")
    compressed = getattr(stat, "UF_COMPRESSED", 0x20) if sys.platform == "darwin" else 0
    if compressed:
        for directory, _dirs, files in os.walk(root, followlinks=False):
            for name in files:
                path = Path(directory) / name
                if getattr(path.lstat(), "st_flags", 0) & compressed:
                    raise ValueError(f"file still uses HFS compression: {path}")
    print(f"rootfs checks passed: {VERSION} ({BUILD}), dyld, launchd, preferences and log stores")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("firmware", help="print the pinned firmware URL and SHA-256")
    commands.add_parser("download", help="download and SHA-256-check the IPSW").add_argument("out")
    extract_parser = commands.add_parser("extract", help="extract the three rootfs DMGs from an IPSW")
    extract_parser.add_argument("ipsw")
    extract_parser.add_argument("outdir")
    commands.add_parser("manifest", help="inspect a standalone BuildManifest.plist").add_argument("path")
    for command in ("prepare", "check"):
        commands.add_parser(command, help=f"{command} a mounted root image").add_argument("root")
    args = parser.parse_args()
    try:
        if args.command == "firmware":
            print(json.dumps({"product": PRODUCT, "board": BOARD, "version": VERSION,
                              "build": BUILD, "url": IPSW_URL, "sha256": IPSW_SHA256}, indent=2))
        elif args.command == "download":
            download(args.out)
        elif args.command == "extract":
            extract(args.ipsw, args.outdir)
        elif args.command == "manifest":
            with open(args.path, "rb") as stream:
                print(json.dumps(manifest_paths(plistlib.load(stream)), indent=2))
        elif args.command == "prepare":
            if os.geteuid() != 0:
                raise ValueError("prepare requires root to preserve numeric iOS owners")
            stage(args.root)
            subprocess.run([sys.executable, str(HERE / "sweet-root-tweaks.py"), args.root], check=True)
            check(args.root)
        else:
            check(args.root)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"rootfs: {exc}\n")


if __name__ == "__main__":
    main()
