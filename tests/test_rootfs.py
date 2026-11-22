#!/usr/bin/env python3
"""Rootfs checks with synthetic IPSWs and a mounted-root layout, no Apple files."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import ios_rootfs as rootfs

spec = importlib.util.spec_from_file_location("root_tweaks", ROOT / "tools/sweet-root-tweaks.py")
tweaks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tweaks)


def plist(root, name, value):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(value))


def manifest():
    identity = {"Info": {"DeviceClass": "d22ap", "RestoreBehavior": "Erase"},
                "Manifest": {component: {"Info": {"Path": f"images/{name}"}}
                             for component, name in rootfs.COMPONENTS.items()}}
    other = copy.deepcopy(identity)
    other["Info"]["DeviceClass"] = "d221ap"
    return {"ProductVersion": "16.7.16", "ProductBuildVersion": "20H392",
            "SupportedProductTypes": ["iPhone10,3", "iPhone10,6"],
            "BuildIdentities": [other, identity]}


def reject(function, *args):
    try:
        function(*args)
    except (ValueError, OSError, zipfile.BadZipFile):
        return
    raise AssertionError("invalid input was accepted")


def test_extract(td):
    archive = td / "firmware.ipsw"
    data = manifest()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
        package.writestr("BuildManifest.plist", plistlib.dumps(data))
        for name in rootfs.manifest_paths(data).values():
            package.writestr(name, name.encode() * 100)
        package.writestr("unrelated.dmg", b"must not be extracted")
    output = td / "extracted"
    digest = rootfs.sha256(archive)
    reject(rootfs.extract, archive, output, "0" * 64)
    assert not output.exists(), "checksum failure created an extraction directory"
    rootfs.extract(archive, output, digest)
    metadata = json.loads((output / "rootfs-source.json").read_text())
    assert metadata["ipsw_sha256"] == digest
    assert set(metadata["components"]) == {"OS.dmg", "SystemOS.dmg", "AppOS.dmg"}
    assert not (output / "unrelated.dmg").exists()
    for name, entry in metadata["components"].items():
        assert rootfs.sha256(output / name) == entry["sha256"]
    reject(rootfs.extract, archive, output, digest)
    for key, value in (("ProductBuildVersion", "wrong"), ("SupportedProductTypes", ["iPhone14,2"])):
        bad = copy.deepcopy(data)
        bad[key] = value
        reject(rootfs.manifest_paths, bad)
    bad = copy.deepcopy(data)
    bad["BuildIdentities"].append(copy.deepcopy(bad["BuildIdentities"][1]))
    reject(rootfs.manifest_paths, bad)
    bad = copy.deepcopy(data)
    bad["BuildIdentities"][1]["Manifest"]["OS"]["Info"]["Path"] = "../outside.dmg"
    reject(rootfs.manifest_paths, bad)


def fixture(root):
    plist(root, "System/Library/CoreServices/SystemVersion.plist",
          {"ProductVersion": "16.7.16", "ProductBuildVersion": "20H392"})
    plist(root, "System/Library/xpc/launchd.plist", {"LaunchDaemons": {
        "keep": {"Label": "com.apple.keep", "_PanicOnCrash": True, "Other": 42},
        "modem": {"Label": "com.apple.CommCenter"}}})
    (root / "sbin").mkdir()
    (root / "sbin/launchd").write_bytes(b"synthetic launchd")
    (root / "sbin/launchd").chmod(0o755)
    os_root = root / "private/preboot/Cryptexes/OS"
    cache = os_root / rootfs.CACHE
    cache.mkdir(parents=True)
    for name in ("dyld_shared_cache_arm64", "dyld_shared_cache_arm64.01", "dyld_shared_cache_arm64.symbols"):
        (cache / name).write_bytes(name.encode())
    (os_root / "usr/lib").mkdir(parents=True)
    (os_root / "usr/lib/dyld").write_bytes(b"synthetic dyld")
    (os_root / "usr/lib/dyld").chmod(0o755)
    (os_root / "usr/lib/test-link").symlink_to("/usr/lib/dyld")
    app_root = root / "private/preboot/Cryptexes/App/Applications/Test.app"
    app_root.mkdir(parents=True)
    (app_root / "Test").write_bytes(b"test app")
    (root / "System/Cryptexes").mkdir()
    (root / "System/Cryptexes/OS").symlink_to("/private/preboot/Cryptexes/OS")
    (root / "usr/lib").mkdir(parents=True)
    (root / "usr/lib/dyld").symlink_to("/System/Cryptexes/OS/usr/lib/dyld")
    plugin = root / "Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin"
    plugin.mkdir(parents=True)
    (plugin / "test").write_bytes(b"plugin")


def test_prepare(td):
    root = td / "root"
    fixture(root)
    before = root / rootfs.PREFS
    assert not before.exists(), "test must exercise missing preferences directory"
    with patch("os.chown") as chown, patch("os.lchown"), \
            patch.object(sys, "argv", ["sweet-root-tweaks.py", str(root)]):
        rootfs.stage(root)
        tweaks.main()
        rootfs.check(root, ownership=False)
        rootfs.stage(root)
        tweaks.main()
        rootfs.check(root, ownership=False)
    jobs = rootfs.load_plist(root, "System/Library/xpc/launchd.plist")["LaunchDaemons"]
    assert set(jobs) == {"keep"} and jobs["keep"]["Other"] == 42
    assert jobs["keep"]["_PanicOnCrasX"] is True
    assert not (root / "Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin").exists()
    assert (root / "Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin/test").is_file()
    for name in ("com.apple.migration", "com.apple.DataMigration"):
        path = root / rootfs.PREFS / f"{name}.plist"
        assert (path.stat().st_mode & 0o777) == 0o600
        assert rootfs.load_plist(root, f"{rootfs.PREFS}/{name}.plist")["LastSystemVersion"] == "20H392"
        assert any(Path(call.args[0]) == path and call.args[1:] == (501, 501)
                   for call in chown.call_args_list)
    assert rootfs.image_path(root, "usr/lib/dyld") == root.resolve() / "private/preboot/Cryptexes/OS/usr/lib/dyld"
    assert os.readlink(root / "usr/lib/test-link") == "/usr/lib/dyld"
    assert os.path.samefile(root / "Applications/Test.app/Test",
                            root / "private/preboot/Cryptexes/App/Applications/Test.app/Test")
    # Existing keybag state must survive a preparation rerun.
    keybag = root / "private/var/keybags/systembag.kb"
    keybag.write_bytes(plistlib.dumps({"KeyBagKeys": b"existing state"}))
    with patch("os.chown"), patch("os.lchown"):
        rootfs.stage(root)
    assert plistlib.loads(keybag.read_bytes())["KeyBagKeys"] == b"existing state"
    (root / "loop").symlink_to("/loop")
    reject(rootfs.image_path, root, "loop")
    plist(root, "System/Library/CoreServices/SystemVersion.plist",
          {"ProductVersion": "17.0", "ProductBuildVersion": "21A329"})
    reject(rootfs.stage, root)


def main():
    with tempfile.TemporaryDirectory() as td:
        test_extract(Path(td))
        test_prepare(Path(td))
    print("test_rootfs: manifest selection, extraction, symlinks, hard links and idempotent preparation passed")


if __name__ == "__main__":
    main()
