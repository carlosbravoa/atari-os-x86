#!/bin/sh
# mk-install-test.sh - a blank internal disk for testing the installer
#
#   scripts/mk-install-test.sh [file] [size]     default build/install-test.img, 8G
#   QEMU_DISK="build/install-test.img" scripts/qemu-test.sh start [bios|uefi]
#
# A sparse, empty file (no partition table) that qemu-test.sh attaches as
# a non-removable SATA disk (QEMU_DISK; ":virtio" or ":nvme" for those) without
# -snapshot, so what the installer writes stays. Booting it afterwards:
#   QEMU_IMG=build/install-test.img QEMU_IMG_AS=ide scripts/qemu-test.sh start [bios|uefi]
# Runs on the host (no Alpine environment needed). Only ever writes the file.

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PROJECT/build/install-test.img}"
SIZE="${2:-8G}"
case "$OUT" in /dev/*) echo "refusing to write to a device: $OUT" >&2; exit 1 ;; esac
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
ls -lsh "$OUT"
