#!/bin/sh
# qemu-test.sh - headless QEMU harness for automated checks
#
#   scripts/qemu-test.sh start [bios|uefi]   boot build/atari-os.img in the background
#   scripts/qemu-test.sh serial "<cmd>"      run a shell command on the serial console (debug image)
#   scripts/qemu-test.sh key <qemu-key>      press a key (QEMU sendkey names, e.g. ret, f1, a)
#   scripts/qemu-test.sh type "<text>"       type text (US layout; \n = RETURN), e.g. "ls h:\n"
#   scripts/qemu-test.sh shot <file.png>     screenshot
#   scripts/qemu-test.sh log [n]             last n lines of the serial log
#   scripts/qemu-test.sh mon "<cmd>"         QEMU monitor command (e.g. device_add for hotplug)
#   scripts/qemu-test.sh stop
#
# Sockets and logs live in $QEMU_TEST_DIR (default: build/qemu-test).
# QEMU_EXTRA adds arguments to the QEMU command line, e.g. a second USB
# stick (scripts/mk-usb-test.sh builds one):
#   QEMU_EXTRA="-drive if=none,id=usb2,format=raw,file=build/usb-test.img
#     -device usb-storage,bus=xhci.0,drive=usb2,removable=on" scripts/qemu-test.sh start
# QEMU_NONET=1 boots without any network card (FujiNet offline checks).

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
	FW=""
	[ "$MODE" = uefi ] && FW="-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd"
	ACCEL="-cpu max"; [ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
	NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:8000-:8000,hostfwd=tcp:127.0.0.1:2222-:22 -device e1000e,netdev=n0"
	[ -n "${QEMU_NONET:-}" ] && NET="-nic none"
	rm -f "$D"/*.sock "$D"/serial.log
	# shellcheck disable=SC2086
	nohup qemu-system-x86_64 $ACCEL $FW -m 1024 -smp 2 -machine q35 \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=stick,format=raw,file="$IMG" \
		-device usb-storage,bus=xhci.0,drive=stick,removable=on \
		-device usb-kbd -vga std \
		$NET \
		-audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
		-chardev socket,id=ser,path="$D/serial.sock",server=on,wait=off,logfile="$D/serial.log" \
		-serial chardev:ser \
		-monitor unix:"$D/mon.sock",server,nowait \
		-snapshot -display none -no-reboot ${QEMU_EXTRA:-} > "$D/qemu.out" 2>&1 &
	echo $! > "$D/qemu.pid"
	echo "started pid $(cat "$D/qemu.pid")"
	;;
serial) py serial "$2" "${3:-2}" ;;
key) py mon "sendkey $2" 0.5 >/dev/null ;;
type) py type "$2" "${QEMU_TYPE_DELAY:-0.15}" ;;
shot) py mon "screendump $2 -f png" 1.5 >/dev/null; ls -la "$2" ;;
log) tail -n "${2:-40}" "$D/serial.log" ;;
mon) py mon "$2" "${3:-1}" | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' -e 's/\r//g' | grep -vF -e "$2" -e "(qemu)" || true ;;
stop) [ -f "$D/qemu.pid" ] && kill "$(cat "$D/qemu.pid")" 2>/dev/null; rm -f "$D/qemu.pid"; echo stopped ;;
*) echo "usage: $0 start|serial|key|type|shot|log|mon|stop" >&2; exit 1 ;;
esac
