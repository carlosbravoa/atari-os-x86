#!/bin/sh
# mk-ventoy-test.sh - build a Ventoy-like test disk (run inside the Alpine env)
#
#   scripts/alpine-env.sh run scripts/mk-ventoy-test.sh
#   QEMU_IMG=build/ventoy-test.img scripts/qemu-test.sh start
#
# Models what a laptop sees when atari-os.img sits as a file on a Ventoy
# stick: the firmware-side loader brings up our kernel and initramfs, but
# the stick's own FAT (modloop, /atari) is only reachable inside the image
# file. Here Limine plays Ventoy's part and boots the kernel directly from a
# data partition that holds nothing of ours except atari-os.img.

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f /etc/alpine-release ] || { echo "error: run me through scripts/alpine-env.sh run $0 (or in an Alpine container)" >&2; exit 1; }
BUILD=$PROJECT/build
IMG=$BUILD/atari-os.img
OUT=$BUILD/ventoy-test.img
STICK=$BUILD/ventoy-stick
[ -f "$IMG" ] || { echo "build atari-os.img first" >&2; exit 1; }
# room for the image file, the kernel and initramfs, and FAT overhead
SIZE_MB=$(( $(stat -c %s "$IMG") / 1048576 + 128 ))

rm -rf "$STICK"
mkdir -p "$STICK/EFI/BOOT" "$STICK/atari-os"
cp /usr/share/limine/BOOTX64.EFI "$STICK/EFI/BOOT/"
cp /usr/share/limine/limine-bios.sys "$STICK/"
cp "$BUILD/stick/atari-os/vmlinuz" "$BUILD/stick/atari-os/initramfs" "$STICK/atari-os/"
cp "$IMG" "$STICK/atari-os.img"
cat > "$STICK/limine.conf" <<EOF
timeout: 0
quiet: yes
/Atari (image file on data partition)
    protocol: linux
    path: boot():/atari-os/vmlinuz
    module_path: boot():/atari-os/initramfs
    cmdline: loglevel=7 console=ttyS0,115200 drm_kms_helper.fbdev_emulation=0 atari.debug=1
EOF

rm -f "$OUT"
truncate -s "${SIZE_MB}M" "$OUT"
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q "$OUT"
OFF=$((2048 * 512))
mformat -i "$OUT@@$OFF" -F -v VENTOYISH ::
mcopy -i "$OUT@@$OFF" -s -Q "$STICK"/* ::
limine bios-install "$OUT" >/dev/null 2>&1
ls -la "$OUT"
