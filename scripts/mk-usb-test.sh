#!/bin/sh
# mk-usb-test.sh - build a small USB drive full of Atari files (run inside the Alpine env)
#
#   scripts/alpine-env.sh run scripts/mk-usb-test.sh
#   QEMU_EXTRA="-drive if=none,id=usb2,format=raw,file=build/usb-test.img \
#     -device usb-storage,bus=xhci.0,drive=usb2,removable=on" scripts/qemu-test.sh start
#
# A 64 MB MBR disk with one FAT partition labelled GAMES, for exercising the
# Library (F9): USB mounting in init, the USB tab, Run, Copy to Library.

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=$PROJECT/build
OUT=$BUILD/usb-test.img
TREE=$BUILD/usb-test
FUJI=$BUILD/fujinet/dist/data
A8=$PROJECT/third_party/atari800

[ -f "$FUJI/autorun.atr" ] || { echo "need $FUJI/autorun.atr (scripts/build-fujinet.sh)" >&2; exit 1; }

rm -rf "$TREE"
mkdir -p "$TREE/GAMES/MORE" "$TREE/DEMOS"
cp "$A8/util/colors.xex" "$TREE/GAMES/HELLO.XEX"
cp "$A8/util/colormix.xex" "$TREE/GAMES/MORE/COLORMIX.XEX"
cp "$FUJI/autorun.atr" "$TREE/DEMOS/A.ATR"
printf 'Test drive for the atari-os-x86 Library (F9).\r\n' > "$TREE/README.TXT"

rm -f "$OUT"
truncate -s 64M "$OUT"
printf 'label: dos\nstart=2048, type=6\n' | sfdisk -q "$OUT"
OFF=$((2048 * 512))
mformat -i "$OUT@@$OFF" -v GAMES ::
mcopy -i "$OUT@@$OFF" -s -Q "$TREE"/* ::
ls -la "$OUT"
