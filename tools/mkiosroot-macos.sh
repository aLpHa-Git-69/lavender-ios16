#!/bin/bash
# Assemble a raw, case-sensitive, journaled HFS+ volume for rd=emmcroot0.
# Usage: sudo bash tools/mkiosroot-macos.sh OS.dmg SystemOS.dmg AppOS.dmg OUT.img [16g]
# Layout follows tools/mkiosroot.sh and the documented bergamota method.
set -euo pipefail

[[ $# -ge 4 && $# -le 5 ]] || { echo "usage: $0 OS.dmg SystemOS.dmg AppOS.dmg OUT.img [16g]" >&2; exit 2; }
[[ $(uname -s) == Darwin ]] || { echo "this builder requires macOS" >&2; exit 1; }
[[ $(id -u) == 0 ]] || { echo "run as root to preserve iOS owners" >&2; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PYTHON3="${PYTHON3:-python3}"
for tool in hdiutil newfs_hfs mount_hfs ditto fsck_hfs mkfile "$PYTHON3"; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done
for image in "$1" "$2" "$3"; do
    [[ -f $image ]] || { echo "missing image: $image" >&2; exit 1; }
done
OUT="$4"
[[ ! -e $OUT && ! -L $OUT ]] || { echo "refusing to overwrite $OUT" >&2; exit 1; }
SIZE="${5:-16g}"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lavender-rootfs.XXXXXX")
DEVICES=()
DEVICE_COUNT=0
COMPLETE=0
CREATED=0
cleanup() {
    local device index busy=0
    for ((index=DEVICE_COUNT-1; index>=0; index--)); do
        device="${DEVICES[index]}"
        if ! hdiutil detach "$device" >/dev/null 2>&1; then
            hdiutil detach -force "$device" >/dev/null 2>&1 || busy=1
        fi
    done
    # Never remove image contents through a mount that could still be live.
    if [[ $busy == 0 ]]; then
        rm -f "$WORK"/*.plist
        rmdir "$WORK"/* "$WORK" 2>/dev/null || true
        if [[ $CREATED == 1 && $COMPLETE == 0 ]]; then rm -f "$OUT"; fi
    else
        echo "detach failed; image and mount directory left at $WORK" >&2
    fi
}
trap cleanup EXIT

attach() {
    local name=$1 device
    shift
    hdiutil attach -plist "$@" > "$WORK/$name.plist"
    device=$("$PYTHON3" -c 'import plistlib,sys; p=plistlib.load(open(sys.argv[1], "rb")); print(next(e["dev-entry"] for e in p["system-entities"] if "dev-entry" in e))' "$WORK/$name.plist")
    [[ $device == /dev/disk* ]] || { echo "unexpected image device: $device" >&2; exit 1; }
    DEVICES+=("$device")
    DEVICE_COUNT=$((DEVICE_COUNT + 1))
    ATTACHED="$device"
}

mount_source() {
    local image=$1 name=$2
    mkdir "$WORK/$name"
    if hdiutil imageinfo "$image" >/dev/null 2>&1; then
        attach "$name" -readonly -nobrowse -owners on -mountpoint "$WORK/$name" "$image"
    else
        attach "$name" -readonly -nobrowse -owners on -imagekey diskimage-class=CRawDiskImage \
            -mountpoint "$WORK/$name" "$image"
    fi
    [[ -d $WORK/$name/System || -d $WORK/$name/usr ]] || { echo "no filesystem mounted for $name" >&2; exit 1; }
}
mount_source "$1" os
mount_source "$2" system
mount_source "$3" app

mkfile -n "$SIZE" "$OUT"
CREATED=1
attach root -nomount -imagekey diskimage-class=CRawDiskImage "$OUT"
ROOT_DEVICE="$ATTACHED"
newfs_hfs -s -J -v iOSRoot "${ROOT_DEVICE/disk/rdisk}"
mkdir "$WORK/root"
mount_hfs -o owners "$ROOT_DEVICE" "$WORK/root"
echo "copying OS into HFSX"
ditto --nohfsCompression --nopreserveHFSCompression "$WORK/os" "$WORK/root"
mkdir -p "$WORK/root/private/preboot/Cryptexes"
echo "copying SystemOS cryptex into HFSX"
ditto --nohfsCompression --nopreserveHFSCompression "$WORK/system" "$WORK/root/private/preboot/Cryptexes/OS"
echo "copying AppOS cryptex into HFSX"
ditto --nohfsCompression --nopreserveHFSCompression "$WORK/app" "$WORK/root/private/preboot/Cryptexes/App"
"$PYTHON3" "$HERE/ios_rootfs.py" prepare "$WORK/root"
df -h "$WORK/root"
sync
umount "$WORK/root"
# Read-only verification is fatal; never publish an image with a broken catalog.
fsck_hfs -fn "${ROOT_DEVICE/disk/rdisk}"
COMPLETE=1
echo "root image built and checked: $OUT"
