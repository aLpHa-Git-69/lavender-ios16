# Rootfs bring-up for lavender

The target is **XNU 8796 / Darwin 22 with iPhone X (D22AP) iOS 16.7.16,
build 20H392**. The rootfs is a **16 GiB, case-sensitive, journaled HFS+
(HFSX) raw volume** in `userdata`, selected by `rd=emmcroot0`.

## Build from Termux

Use the manual `rootfs.yml` workflow in your GitHub fork. Its macOS runner
mounts Apple's APFS images and writes HFSX with numeric iOS ownership. Root
access on an Android host is useful for USB flashing, but the local image
builder also needs HFS+ kernel support and APFS/HFS+ tools.

```bash
# In this checkout, with gh authenticated to your fork:
gh workflow run rootfs.yml --ref main
gh run list --workflow rootfs.yml --limit 5
gh run watch <run-id> --exit-status
gh run download <run-id> -n lavender-rootfs-20H392 -D artifacts/rootfs
```

Verify the downloaded files from their directory:

```bash
sha256sum -c SHA256SUMS
```

The artifact contains `ios16-root.simg`, `SHA256SUMS` and
`rootfs-source.json`. The JSON records the IPSW and each extracted DMG's
hash. The raw image is built and checked on the runner; its sparse image is
the file to flash. Artifacts are retained for three days.

### Firmware input

The build downloads the matching IPSW directly from Apple's CDN and checks
its SHA-256 before extracting any rootfs files:

```
ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb
```

Inspect the pinned URL and version with:

```bash
python3 tools/ios_rootfs.py firmware
```

The extractor selects the `d22ap` **erase** identity in `BuildManifest.plist`.
It selects components by their manifest keys rather than guessing DMG names:

| Component | Archive member for 20H392 | Output |
| --- | --- | --- |
| `OS` | `087-86698-044.dmg` | `OS.dmg` |
| `Cryptex1,SystemOS` | `087-86593-047.dmg` | `SystemOS.dmg` |
| `Cryptex1,AppOS` | `087-86715-047.dmg` | `AppOS.dmg` |

The IPSW is approximately 6.17 GB. The runner needs room for the IPSW, the
extracted DMGs, the assembled root image and its sparse output.

## Local macOS build

```bash
mkdir -p work
python3 tools/ios_rootfs.py download ios16.ipsw
python3 tools/ios_rootfs.py extract ios16.ipsw work/ios16-input
sudo env "PYTHON3=$(command -v python3)" bash tools/mkiosroot-macos.sh \
  work/ios16-input/OS.dmg work/ios16-input/SystemOS.dmg \
  work/ios16-input/AppOS.dmg ios16-root.img 16g
python3 tools/img2simg.py ios16-root.img ios16-root.simg
```

An already downloaded, verified IPSW can be supplied directly to `extract`.
Both builders refuse to overwrite their output images.

On a Linux PC with `apfs-fuse`, `mkfs.hfsplus`, `fsck.hfsplus`, `rsync` and
privileged HFS+ loop mounts, use `tools/mkiosroot.sh` with the same four
image arguments and `16G` as the size. It applies the same preparation and
checks before unmounting.

## What preparation does

- Copies OS and both cryptexes with decompressed file contents and preserved
  owners, modes and symlinks.
- Stores the cryptexes under `/private/preboot/Cryptexes/{OS,App}` and
  hard-links their shared cache and missing userland files into the root.
- Creates logd's stores and `.migrated` markers with owner `272:272`.
- Creates the bring-up system keybag and required container directories.
- Applies `sweet-root-tweaks.py`: daemon panic mitigation, CommCenter
  removal, the audio workaround, SpringBoard watchdog preferences and Setup
  preferences.
- Writes **both** `com.apple.migration.plist` and
  `com.apple.DataMigration.plist` with `LastSystemVersion=20H392`, owner
  `501:501`, mode `0600`.
- Checks the version, launchd, dyld/cache hard links, preferences, containers,
  log markers and remaining HFS-compressed files, then runs `fsck_hfs`.

The first device boot still has to initialize data containers. These checks
establish a prepared image; the boot log establishes whether your kernel
and device successfully run it.

## Connect the powered-off lavender

Use the rooted second Android phone as the USB host with an OTG adapter.
Boot lavender into fastboot with **Power + Volume Down**. Termux needs the
`android-tools` package. Check the USB link first:

```bash
su -c "$PREFIX/bin/fastboot devices"
```

Flashing the rootfs replaces `userdata`; use your verified backup/restore
path from `docs/RESTORE.md`. From the repository directory:

```bash
su -c "$PREFIX/bin/fastboot -S 512M flash userdata $PWD/artifacts/rootfs/ios16-root.simg"
```

Use your lavender shim with the matching root arguments. The repository's
working bring-up configuration includes:

```
rd=emmcroot0 emmcrw=1 rootrw=1
cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1
sweetlog=1 cpus=4 wdt=-1 wdt_test=no_user_checkin
sweetouchi2c=1 sweetouchwait=0
```

`cpus=4` selects the four A73 Gold cores for this 16 KiB-page port. Boot the
shim from `recovery` with **Power + Volume Up**; see `docs/BOOT.md` for this
board's recovery behavior. The booting kernel and its shim must select the
eMMC root above.

## Debug the first boot

Record the kernel hash, shim payload command line and decoded log for each
attempt. The first useful milestones are:

1. `BSD root: emmcroot0` — the driver selected the root partition.
2. `hfs: mounted iOSRoot` — the filesystem mounted.
3. `exec launchd` — iOS PID 1 started.
4. `Early boot complete` — the launchd boot phase completed.
5. `exec backboardd` / `exec SpringBoard` — graphical userland started.

With lavender in TWRP, the rooted Termux host can pull the persistent log:

```bash
su -c "$PREFIX/bin/adb push $PWD/tools/pull-sweetlog.sh /tmp/"
su -c "$PREFIX/bin/adb shell sh /tmp/pull-sweetlog.sh"
su -c "$PREFIX/bin/adb pull /tmp/sweetlog.bin $PWD/sweetlog.bin"
python3 tools/sweet-log.py sweetlog.bin boot.txt
```

Share `boot.txt`, the last visible boot message and your kernel build/patch
revision. See `docs/TROUBLESHOOTING.md` for CMD24/CMD25 timeouts,
`CS_KILLED`, migration delays and the known single-volume preboot messages.
