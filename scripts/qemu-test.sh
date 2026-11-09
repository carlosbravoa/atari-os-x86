#!/bin/sh
# qemu-test.sh - headless QEMU harness for automated checks
#
#   scripts/qemu-test.sh start [bios|uefi|uefi32]  boot build/atari-os.img (or $QEMU_IMG) in the background
#                                            (uefi32: 32-bit UEFI firmware from build/ovmf32/)
#   scripts/qemu-test.sh wait [seconds]      wait until atari800 runs (debug image; default 180 s, 600 s without KVM)
#   scripts/qemu-test.sh serial "<cmd>"      run a shell command on the serial console (debug image)
#   scripts/qemu-test.sh key <qemu-key>      press a key (QEMU sendkey names, e.g. ret, f1, a)
#   scripts/qemu-test.sh type "<text>"       type text (US layout; \n = RETURN), e.g. "ls h:\n"
#   scripts/qemu-test.sh shot <file.png>     screenshot
#   scripts/qemu-test.sh log [n]             last n lines of the serial log
#   scripts/qemu-test.sh mon "<cmd>"         QEMU monitor command (e.g. device_add for hotplug)
#   scripts/qemu-test.sh stop
#   scripts/qemu-test.sh debug-image <img>   make a release image boot with the serial debug shell
#                                            (edits its limine.conf in place; needs mtools)
#
# QEMU_IMG picks the image (default build/atari-os.img); OVMF_CODE the UEFI
# firmware (default: the first of the usual Debian/Ubuntu/Fedora/Arch paths).
# Sockets and logs live in $QEMU_TEST_DIR (default: build/qemu-test).
# QEMU_EXTRA adds arguments to the QEMU command line, e.g. a second USB
# stick (scripts/mk-usb-test.sh builds one):
#   QEMU_EXTRA="-drive if=none,id=usb2,format=raw,file=build/usb-test.img
#     -device usb-storage,bus=xhci.0,drive=usb2,removable=on" scripts/qemu-test.sh start
# QEMU_NONET=1 boots without any network card (FujiNet offline checks).
# uefi32 is 32-bit UEFI firmware (Atom tablets): build/ovmf32/OVMF_CODE.4m.fd
# and OVMF_VARS.4m.fd, QEMU's edk2-i386-code.fd/-vars.fd from Alpine's
# qemu-system-i386 and qemu-edk2-i386-vars packages (docs/specs/firmware-2.md B.3).
# The 32-bit image (docs/specs/i686-image.md): QEMU_SYSTEM picks the QEMU
# binary (default qemu-system-x86_64), QEMU_CPU the CPU model (default: the
# host's with KVM, else max), QEMU_MEM the RAM in MB (default 1024). A
# model's missing instructions (SSE2 on pentium3) still run, under KVM and
# under QEMU's own CPU emulation alike: it tests the kernel, not userspace.
#   QEMU_IMG=build/atari-os-i686.img QEMU_SYSTEM=qemu-system-i386 QEMU_CPU=n270 \
#     QEMU_MEM=512 scripts/qemu-test.sh start bios
#
# The installer (F8 "Install onto this laptop's disk") needs internal disks
# that survive the run: everything else is -snapshot, but each entry of
# QEMU_DISK ("path[:ide|virtio|nvme]", space separated; default ide, i.e.
# SATA on q35) is attached as a non-removable disk with snapshot=off.
# scripts/mk-install-test.sh makes a blank one:
#   QEMU_DISK="build/install-test.img" scripts/qemu-test.sh start uefi
# QEMU_IMG_AS=ide|virtio|nvme attaches the main image as an internal disk instead
# of a USB stick (booting an installed disk). QEMU_VARS=<file> gives OVMF a
# persistent variable store (created from OVMF_VARS_4M.fd if missing), so
# firmware boot entries outlive the run.
#
# QEMU_KERNEL=1 boots without any stick: QEMU loads kernel and initramfs
# itself (-kernel/-initrd from $QEMU_KERNEL_DIR, default build/stick/atari-os)
# with the first cmdline: of build/stick/limine.conf plus
# "console=ttyS0,115200 atari.debug=1" and $QEMU_APPEND. That is how a
# boot loader on the host disk starts us (the loopback install; the disk
# from scripts/mk-hostfs-test.sh goes in QEMU_DISK):
#   QEMU_KERNEL=1 QEMU_DISK=build/hostfs-test.img \
#     QEMU_APPEND="atari.image=<uuid>:/atari/atari-os-x86.img" scripts/qemu-test.sh start

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${QEMU_IMG:-$PROJECT/build/atari-os.img}"
D="${QEMU_TEST_DIR:-$PROJECT/build/qemu-test}"
mkdir -p "$D"

py() { python3 -I - "$D" "$@" <<'PY'
import socket, sys, time
D, what = sys.argv[1], sys.argv[2]
def talk(name, data, wait):
    s = socket.socket(socket.AF_UNIX); s.connect(f"{D}/{name}.sock"); s.settimeout(wait)
    time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
    s.sendall(data.encode()); time.sleep(wait)
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk: break
            out += chunk
    except Exception: pass
    s.close(); return out.decode(errors="replace")
if what == "mon":
    print(talk("mon", sys.argv[3] + "\n", float(sys.argv[4])))
elif what == "type":
    # one sendkey per character, US layout, as the emulator maps PC keys
    names = {" ": "spc", "\n": "ret", "-": "minus", "_": "shift-minus", "=": "equal",
             "+": "shift-equal", ";": "semicolon", ":": "shift-semicolon",
             "'": "apostrophe", '"': "shift-apostrophe", ",": "comma", "<": "shift-comma",
             ".": "dot", ">": "shift-dot", "/": "slash", "?": "shift-slash",
             "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5",
             "^": "shift-6", "&": "shift-7", "*": "shift-8", "(": "shift-9", ")": "shift-0",
             "[": "bracket_left", "]": "bracket_right", "\\": "backslash", "|": "shift-backslash"}
    text = sys.argv[3].replace("\\n", "\n")
    s = socket.socket(socket.AF_UNIX); s.connect(f"{D}/mon.sock"); s.settimeout(0.05)
    for ch in text:
        if ch.isalpha() and ch.isascii():
            key = ("shift-" + ch.lower()) if ch.isupper() else ch
        elif ch.isdigit():
            key = ch
        elif ch in names:
            key = names[ch]
        else:
            print(f"type: no key for {ch!r}", file=sys.stderr); continue
        s.sendall(f"sendkey {key}\n".encode())
        time.sleep(float(sys.argv[4]))
        try: s.recv(65536)
        except Exception: pass
    s.close()
elif what == "serial":
    out = talk("serial", "echo __BEGIN__; " + sys.argv[3] + "; echo __END__\n", float(sys.argv[4]))
    lines = out.replace("\r", "").split("\n")
    body, on = [], False
    for l in lines:
        if l.strip() == "__END__" and on: break
        if on: body.append(l)
        if l.strip() == "__BEGIN__": on = True
    print("\n".join(body))
PY
}

case "${1:-}" in
start)
	MODE="${2:-bios}"
	if [ -n "${QEMU_KERNEL:-}" ]; then
		KDIR="${QEMU_KERNEL_DIR:-$PROJECT/build/stick/atari-os}"
		[ -f "$KDIR/vmlinuz" ] && [ -f "$KDIR/initramfs" ] || { echo "no vmlinuz/initramfs in $KDIR (QEMU_KERNEL_DIR)" >&2; exit 1; }
		KCONF="$KDIR/../limine.conf"
		KCMD=$(sed -n 's/^[[:space:]]*cmdline:[[:space:]]*//p' "$KCONF" 2>/dev/null | head -n 1)
		KCMD="$KCMD console=ttyS0,115200 atari.debug=1 ${QEMU_APPEND:-}"
	else
		[ -f "$IMG" ] || { echo "no image at $IMG (QEMU_IMG)" >&2; exit 1; }
	fi
	FW=""
	# uefi32: 32-bit UEFI on the 64-bit CPU (Bay Trail tablets), from
	# build/ovmf32/ (Debian's ovmf-ia32, see docs/specs/firmware-2.md)
	O32=$PROJECT/build/ovmf32
	case "$MODE" in
		uefi) CODES="/usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/ovmf-x86_64-code.bin"
			VARS="/usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/ovmf/OVMF_VARS.fd /usr/share/edk2/x64/OVMF_VARS.4m.fd" ;;
		uefi32) CODES="$O32/OVMF32_CODE_4M.fd $O32/OVMF_CODE.4m.fd"; VARS="$O32/OVMF32_VARS_4M.fd $O32/OVMF_VARS.4m.fd" ;;
		bios) ;;
		*) echo "start: bios, uefi or uefi32" >&2; exit 1 ;;
	esac
	if [ "$MODE" != bios ]; then
		for f in ${OVMF_CODE:-} $CODES; do
			[ -f "$f" ] && { FW="-drive if=pflash,format=raw,readonly=on,file=$f"; break; }
		done
		[ -n "$FW" ] || { echo "no OVMF firmware found (install ovmf or set OVMF_CODE)" >&2; exit 1; }
		if [ -n "${QEMU_VARS:-}" ]; then
			if [ ! -f "$QEMU_VARS" ]; then
				for f in ${OVMF_VARS:-} $VARS; do
					[ -f "$f" ] && { cp "$f" "$QEMU_VARS"; break; }
				done
			fi
			[ -f "$QEMU_VARS" ] || { echo "no OVMF variable store template (set OVMF_VARS)" >&2; exit 1; }
			FW="$FW -drive if=pflash,format=raw,snapshot=off,file=$QEMU_VARS"
		fi
	fi
	# SeaBIOS only tries the first hard disk: the image must be it. OVMF
	# gets no bootindex, which would make it rewrite BootOrder from QEMU's.
	BI=""; [ "$MODE" != bios ] || BI=",bootindex=0"
	case "${QEMU_IMG_AS:-usb}" in
		usb) MAIN="-device usb-storage,bus=xhci.0,drive=stick,removable=on$BI" ;;
		ide) MAIN="-device ide-hd,drive=stick$BI" ;;
		virtio) MAIN="-device virtio-blk-pci,drive=stick$BI" ;;
		nvme) MAIN="-device nvme,drive=stick,serial=atari-main$BI" ;;
		*) echo "QEMU_IMG_AS: usb, ide, virtio or nvme" >&2; exit 1 ;;
	esac
	DISKS="" i=0
	for spec in ${QEMU_DISK:-}; do
		file=${spec%%:*} bus=ide
		[ "$file" = "$spec" ] || bus=${spec##*:}
		[ -f "$file" ] || { echo "QEMU_DISK: no file $file (scripts/mk-install-test.sh)" >&2; exit 1; }
		case "$bus" in
			ide) dev="ide-hd,drive=hd$i" ;;
			virtio) dev="virtio-blk-pci,drive=hd$i" ;;
			nvme) dev="nvme,drive=hd$i,serial=atari$i" ;;
			*) echo "QEMU_DISK: $spec: bus must be ide, virtio or nvme" >&2; exit 1 ;;
		esac
		DISKS="$DISKS -drive if=none,id=hd$i,format=raw,snapshot=off,file=$file -device $dev"
		i=$((i + 1))
	done
	STICK="-drive if=none,id=stick,format=raw,file=$IMG $MAIN"
	[ -z "${QEMU_KERNEL:-}" ] || STICK=""
	ACCEL="-cpu max"; [ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
	[ -z "${QEMU_CPU:-}" ] || { ACCEL="-cpu $QEMU_CPU"; [ -w /dev/kvm ] && ACCEL="-enable-kvm $ACCEL"; }
	NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:8000-:8000,hostfwd=tcp:127.0.0.1:2222-:22 -device e1000e,netdev=n0"
	[ -n "${QEMU_NONET:-}" ] && NET="-nic none"
	rm -f "$D"/*.sock "$D"/serial.log
	set --
	[ -z "${QEMU_KERNEL:-}" ] || set -- -kernel "$KDIR/vmlinuz" -initrd "$KDIR/initramfs" -append "$KCMD"
	# shellcheck disable=SC2086
	nohup "${QEMU_SYSTEM:-qemu-system-x86_64}" $ACCEL $FW -m "${QEMU_MEM:-1024}" -smp 2 -machine q35 \
		-device qemu-xhci,id=xhci \
		$STICK $DISKS "$@" \
		-device usb-kbd -vga std \
		$NET \
		-audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
		-chardev "socket,id=ser,path=$D/serial.sock,server=on,wait=off,logfile=$D/serial.log" \
		-serial chardev:ser \
		-monitor unix:"$D/mon.sock",server,nowait \
		-snapshot -display none -no-reboot ${QEMU_EXTRA:-} > "$D/qemu.out" 2>&1 &
	echo $! > "$D/qemu.pid"
	echo "started pid $(cat "$D/qemu.pid")"
	;;
wait)
	LIMIT="${2:-}"
	[ -n "$LIMIT" ] || { if [ -w /dev/kvm ]; then LIMIT=180; else LIMIT=600; fi; }
	T0=$(date +%s)
	while :; do
		ELAPSED=$(( $(date +%s) - T0 ))
		if [ ! -f "$D/qemu.pid" ] || ! kill -0 "$(cat "$D/qemu.pid")" 2>/dev/null; then
			echo "QEMU is not running (see $D/qemu.out)" >&2; exit 1
		fi
		PID=$(py serial "pidof atari800" 2 | tr -dc '0-9 ' | awk '{print $1}')
		if [ -n "$PID" ]; then echo "atari800 running (pid $PID) after ${ELAPSED}s"; exit 0; fi
		[ "$ELAPSED" -lt "$LIMIT" ] || { echo "atari800 not running after ${LIMIT}s" >&2; exit 1; }
		sleep 3
	done
	;;
debug-image)
	F="${2:?usage: $0 debug-image <img>}"
	command -v mcopy >/dev/null 2>&1 || { echo "debug-image needs mtools" >&2; exit 1; }
	export MTOOLS_SKIP_CHECK=1
	PART="$F@@$((2048 * 512))"   # the FAT partition mkimage.sh makes
	TMP="$D/limine.conf.debug"
	mtype -i "$PART" ::limine.conf > "$TMP"
	if grep -m1 'cmdline:' "$TMP" | grep -q 'atari.debug=1'; then
		echo "$F already boots the debug shell"
	else
		# first boot entry = the default one (timeout: 0)
		awk '!done && /^[[:space:]]*cmdline:/ { $0 = $0 " console=ttyS0,115200 atari.debug=1"; done = 1 } { print }' \
			"$TMP" > "$TMP.new"
		mcopy -o -i "$PART" "$TMP.new" ::limine.conf
		echo "$F: default entry now boots with console=ttyS0,115200 atari.debug=1"
	fi
	rm -f "$TMP" "$TMP.new"
	;;
serial) py serial "$2" "${3:-2}" ;;
key) py mon "sendkey $2" 0.5 >/dev/null ;;
type) py type "$2" "${QEMU_TYPE_DELAY:-0.15}" ;;
shot)
	case "$2" in /*) F="$2" ;; *) F="$PWD/$2" ;; esac # QEMU resolves it in its own cwd
	py mon "screendump $F -f png" 1.5 >/dev/null; ls -la "$F" ;;
log) tail -n "${2:-40}" "$D/serial.log" ;;
mon) py mon "$2" "${3:-1}" | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' -e 's/\r//g' | grep -vF -e "$2" -e "(qemu)" || true ;;
stop)
	# wait for it to go: an exiting QEMU unlinks its sockets, which may
	# already be the next run's
	if [ -f "$D/qemu.pid" ] && P=$(cat "$D/qemu.pid") && kill "$P" 2>/dev/null; then
		i=0; while kill -0 "$P" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	fi
	rm -f "$D/qemu.pid"; echo stopped ;;
*) echo "usage: $0 start|wait|serial|key|type|shot|log|mon|stop|debug-image" >&2; exit 1 ;;
esac
