#!/bin/sh
# run-qemu.sh - boot build/atari-os.img in QEMU as a USB stick
#
#   scripts/run-qemu.sh [bios|uefi] [extra qemu args...]
#
# The image is attached as a USB mass-storage device behind an xHCI
# controller, which is how a real laptop sees it. Serial console goes to
# the terminal (pick the debug entry in Limine to get a shell there).

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$PROJECT/build/atari-os.img"
MODE="${1:-bios}"
[ $# -gt 0 ] && shift

[ -f "$IMG" ] || { echo "no image; run: scripts/alpine-env.sh run scripts/mkimage.sh" >&2; exit 1; }

ACCEL=""
if [ -w /dev/kvm ]; then
	ACCEL="-enable-kvm -cpu host"
else
	echo "note: /dev/kvm not writable, running without KVM (slow)" >&2
	ACCEL="-cpu max"
fi

FW=""
if [ "$MODE" = uefi ]; then
	for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/ovmf/OVMF.fd; do
		[ -f "$f" ] && { FW="-drive if=pflash,format=raw,readonly=on,file=$f"; break; }
	done
	[ -n "$FW" ] || { echo "no OVMF firmware found (apt install ovmf)" >&2; exit 1; }
fi

AUDIO="-audiodev pipewire,id=snd0"
if [ -n "${PULSE_SERVER:-}" ] || [ ! -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pipewire-0" ]; then
	AUDIO="-audiodev pa,id=snd0"
fi

# shellcheck disable=SC2086
exec qemu-system-x86_64 $ACCEL $FW \
	-m 1024 -smp 2 \
	-machine q35 \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=stick,format=raw,file="$IMG" \
	-device usb-storage,bus=xhci.0,drive=stick,removable=on \
	-device usb-kbd \
	-netdev user,id=n0,hostfwd=tcp:127.0.0.1:8000-:8000 -device e1000e,netdev=n0 \
	-vga std \
	$AUDIO -device intel-hda -device hda-output,audiodev=snd0 \
	-serial stdio \
	-display gtk,gl=off \
	"$@"
