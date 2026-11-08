#!/bin/sh
# mk-hostfs-test.sh - a "host Linux" disk for testing the loopback install
#
#   scripts/mk-hostfs-test.sh [image] [out]   default build/atari-os.img, build/hostfs-test.img
#
# A 4 GiB sparse file holding one filesystem (no partition table) with
# the image at /atari/atari-os-x86.img, as scripts/install-linux.sh --image
# leaves it. HOSTFS_TYPE=ext4 (default; mkfs.ext4 -d, e2fsprogs, no root
# needed) or btrfs (mkfs.btrfs --rootdir, btrfs-progs: e.g. inside the
# Alpine env after apk add btrfs-progs). HOSTFS_UUID sets the filesystem
# UUID (default a7a12000-0000-4000-8000-000000000001). Boot it without a
# stick, kernel and initramfs straight from build/stick/:
#
#   QEMU_KERNEL=1 QEMU_DISK=build/hostfs-test.img \
#     QEMU_APPEND="atari.image=a7a12000-0000-4000-8000-000000000001:/atari/atari-os-x86.img" \
#     scripts/qemu-test.sh start bios
#
# Only ever writes the output file (and build/hostfs-test.d/, its staging).

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$PROJECT/build/atari-os.img}"
OUT="${2:-$PROJECT/build/hostfs-test.img}"
TYPE="${HOSTFS_TYPE:-ext4}"
UUID="${HOSTFS_UUID:-a7a12000-0000-4000-8000-000000000001}"
STAGE="$PROJECT/build/hostfs-test.d"
case "$OUT" in /dev/*) echo "refusing to write to a device: $OUT" >&2; exit 1 ;; esac
[ -f "$IMG" ] || { echo "no image $IMG (scripts/mkimage.sh)" >&2; exit 1; }
rm -rf "$STAGE"
mkdir -p "$STAGE/atari" "$STAGE/home/user"
cp --sparse=always "$IMG" "$STAGE/atari/atari-os-x86.img" 2>/dev/null || cp "$IMG" "$STAGE/atari/atari-os-x86.img"
echo "a file of the host system" > "$STAGE/home/user/notes.txt"
rm -f "$OUT"
truncate -s 4G "$OUT"
case "$TYPE" in
	ext4) mkfs.ext4 -q -F -L hostfs -U "$UUID" -d "$STAGE" "$OUT" ;;
	btrfs) mkfs.btrfs -q -f -L hostfs -U "$UUID" --rootdir "$STAGE" "$OUT" ;;
	*) echo "HOSTFS_TYPE: ext4 or btrfs" >&2; exit 1 ;;
esac
rm -rf "$STAGE"
ls -lsh "$OUT"
echo "UUID $UUID; boot with atari.image=$UUID:/atari/atari-os-x86.img"
