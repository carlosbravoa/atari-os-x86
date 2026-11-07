#!/bin/sh
# mkimage.sh - assemble the bootable atari-os-x86 image (run inside the Alpine env)
#
#   scripts/alpine-env.sh run scripts/mkimage.sh
#
# Produces build/atari-os.img: an MBR disk with one FAT32 partition that
# boots on BIOS and UEFI through Limine. Layout of the stick:
#
#   /EFI/BOOT/BOOTX64.EFI     UEFI loader (Limine)
#   /limine-bios.sys          BIOS loader stage (plus code in the MBR)
#   /limine.conf
#   /atari-os/vmlinuz         Alpine linux-lts
#   /atari-os/initramfs       our init + emulator + minimal driver set
#   /atari-os/modloop.sqfs    full module set + firmware (squashfs)
#   /atari/                   the user's area: disks, carts, roms, config
#
# Everything above /atari is replaceable by copying a new build over it.

set -eu
WORK=/work
BUILD=$WORK/build
STAGE=$BUILD/stage          # packages unpacked here
INITRAMFS=$BUILD/initramfs  # initramfs tree
OUT=$BUILD/atari-os.img
IMG_MB=${IMG_MB:-1024}

# fbdev_emulation=0: no fbcon on the panel, ever. The emulator owns the
# display; a late fbcon takeover otherwise commits over its scanout.
CMDLINE="quiet loglevel=3 drm_kms_helper.fbdev_emulation=0 vt.global_cursor_default=0 ${EXTRA_CMDLINE:-}"

# Firmware: GPUs, Intel Wi-Fi (linux-firmware-intel carries iwlwifi), the
# SOF audio DSP on 2019+ Intel laptops, and the wireless regulatory db.
FIRMWARE_PKGS="linux-firmware-i915 linux-firmware-radeon linux-firmware-nvidia linux-firmware-amdgpu
	linux-firmware-intel sof-firmware wireless-regdb"

# Modules that must be inside the initramfs: enough to see the stick and
# the keyboard before the modloop is available. Dependencies are resolved.
INITRAMFS_MODULES="evdev usbhid hid-generic xhci-pci xhci-hcd ehci-pci ehci-hcd
	ohci-pci ohci-hcd uhci-hcd usb-storage uas sd_mod ahci nvme mmc_block sdhci-pci
	loop squashfs vfat nls_cp437 nls_iso8859-1 nls_utf8 exfat ntfs3 button
	virtio_pci virtio_blk virtio_input virtio_scsi bochs af_packet"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. unpack packages into the stage ------------------------------------
if [ "${SKIP_STAGE:-0}" = 1 ] && [ -d "$STAGE/lib/modules" ]; then
	log "Reusing $STAGE"
else
log "Fetching packages into $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
# shellcheck disable=SC2086
apk add --root "$STAGE" --initdb --no-scripts --no-cache \
	--keys-dir /etc/apk/keys --repositories-file /etc/apk/repositories \
	linux-lts musl busybox-static alsa-lib libdrm \
	expat libcrypto3 zlib libstdc++ libgcc ca-certificates-bundle $FIRMWARE_PKGS >/dev/null
fi
KVER=$(ls "$STAGE/lib/modules")
[ -n "$KVER" ] || die "no kernel modules in stage"
log "Kernel $KVER"

# ---- 2. initramfs ---------------------------------------------------------
log "Building initramfs"
rm -rf "$INITRAMFS"
mkdir -p "$INITRAMFS"/bin "$INITRAMFS"/lib "$INITRAMFS"/usr/bin "$INITRAMFS"/usr/lib \
	"$INITRAMFS"/usr/share/alsa "$INITRAMFS"/proc "$INITRAMFS"/sys "$INITRAMFS"/dev \
	"$INITRAMFS"/run "$INITRAMFS"/media "$INITRAMFS"/etc "$INITRAMFS"/tmp "$INITRAMFS"/root

gcc -static -Os -Wall -Wextra -o "$INITRAMFS/init" "$WORK/init/init.c"
strip "$INITRAMFS/init"

cp "$STAGE/bin/busybox.static" "$INITRAMFS/bin/busybox"
cp "$BUILD/emulator/src/atari800" "$INITRAMFS/usr/bin/atari800"
strip "$INITRAMFS/usr/bin/atari800"
cp "$STAGE/lib/ld-musl-x86_64.so.1" "$INITRAMFS/lib/"
cp "$STAGE/usr/lib/libasound.so.2" "$STAGE/usr/lib/libdrm.so.2" "$INITRAMFS/usr/lib/"
# alsa-lib needs its configuration tree to resolve "default" (~300 KB)
cp -r "$STAGE/usr/share/alsa/." "$INITRAMFS/usr/share/alsa/"
rm -rf "$INITRAMFS/usr/share/alsa/ucm2" "$INITRAMFS/usr/share/alsa/topology"
# busybox applets as symlinks: used by init (modprobe) and the debug shell
for a in $("$STAGE/bin/busybox.static" --list); do ln -sf busybox "$INITRAMFS/bin/$a"; done

# FujiNet-PC: binary, web UI data, default config; plus its shared libraries.
if [ -x "$BUILD/fujinet/dist/fujinet" ]; then
	mkdir -p "$INITRAMFS/usr/lib/fujinet" "$INITRAMFS/etc/ssl/certs"
	cp "$BUILD/fujinet/dist/fujinet" "$INITRAMFS/usr/lib/fujinet/"
	strip "$INITRAMFS/usr/lib/fujinet/fujinet"
	cp -r "$BUILD/fujinet/dist/data" "$INITRAMFS/usr/lib/fujinet/"
	cp "$BUILD/fujinet/dist/fnconfig.ini" "$INITRAMFS/usr/lib/fujinet/"
	for lib in libexpat.so.1 libcrypto.so.3 libz.so.1 libstdc++.so.6 libgcc_s.so.1; do
		cp "$STAGE/usr/lib/$lib" "$INITRAMFS/usr/lib/"
	done
	cp "$STAGE/etc/ssl/certs/ca-certificates.crt" "$INITRAMFS/etc/ssl/certs/"
	ln -sf certs/ca-certificates.crt "$INITRAMFS/etc/ssl/cert.pem"
	log "FujiNet-PC included"
else
	log "FujiNet-PC not built (scripts/build-fujinet.sh); image will be emulator-only"
fi

# udhcpc configures the interface and resolv.conf through this script.
cat > "$INITRAMFS/etc/udhcpc.script" <<'EOS'
#!/bin/sh
# udhcpc event script for atari-os-x86 (busybox applets only)
case "$1" in
	deconfig)
		busybox ip addr flush dev "$interface"
		;;
	bound|renew)
		busybox ip addr flush dev "$interface"
		busybox ip addr add "$ip/${mask:-24}" dev "$interface"
		[ -n "$router" ] && busybox ip route replace default via "${router%% *}" dev "$interface"
		: > /etc/resolv.conf
		for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf; done
		[ -n "$domain" ] && echo "search $domain" >> /etc/resolv.conf
		echo "udhcpc: $interface $ip via ${router:-?} dns ${dns:-?}" > /dev/kmsg
		;;
esac
exit 0
EOS
chmod +x "$INITRAMFS/etc/udhcpc.script"
echo "atari" > "$INITRAMFS/etc/hostname"
printf '127.0.0.1 localhost atari\n' > "$INITRAMFS/etc/hosts"

# Module subset with dependencies, from modules.dep.
MODDIR="$STAGE/lib/modules/$KVER"
mkdir -p "$INITRAMFS/lib/modules/$KVER"
cp "$MODDIR"/modules.* "$INITRAMFS/lib/modules/$KVER/"
resolve_mods() {
	# prints relative paths of the named modules and their dependencies
	for m in "$@"; do
		# module names use '-' or '_' interchangeably
		pat=$(echo "$m" | sed 's/[-_]/[-_]/g')
		grep -E "/${pat}\.ko(\.gz|\.xz|\.zst)?:" "$MODDIR/modules.dep" || true
	done | tr ':' ' ' | tr -s ' ' '\n' | sed '/^$/d' | sort -u
}
# shellcheck disable=SC2086
resolve_mods $INITRAMFS_MODULES | while read -r rel; do
	mkdir -p "$INITRAMFS/lib/modules/$KVER/$(dirname "$rel")"
	cp "$MODDIR/$rel" "$INITRAMFS/lib/modules/$KVER/$rel"
done
log "initramfs modules: $(find "$INITRAMFS/lib/modules" -name '*.ko*' | wc -l)"

(cd "$INITRAMFS" && find . | cpio -o -H newc --quiet | zstd -T0 -19 -q -o "$BUILD/initramfs.zst" -f)
ls -la "$BUILD/initramfs.zst"

# ---- 3. modloop -----------------------------------------------------------
log "Building modloop (modules + firmware)"
rm -rf "$BUILD/modloop"
mkdir -p "$BUILD/modloop/modules" "$BUILD/modloop/firmware"
cp -a "$STAGE/lib/modules/$KVER" "$BUILD/modloop/modules/"
[ -d "$STAGE/lib/firmware" ] && cp -a "$STAGE/lib/firmware/." "$BUILD/modloop/firmware/"
rm -f "$BUILD/modloop.sqfs"
mksquashfs "$BUILD/modloop" "$BUILD/modloop.sqfs" -comp zstd -Xcompression-level 15 -all-root -no-progress -quiet
ls -la "$BUILD/modloop.sqfs"

# ---- 4. stick content -----------------------------------------------------
log "Assembling stick content"
STICK=$BUILD/stick
rm -rf "$STICK"
mkdir -p "$STICK/EFI/BOOT" "$STICK/atari-os" "$STICK/atari/disks" "$STICK/atari/carts" \
	"$STICK/atari/roms" "$STICK/atari/config"
cp /usr/share/limine/BOOTX64.EFI "$STICK/EFI/BOOT/"
cp /usr/share/limine/limine-bios.sys "$STICK/"
cp "$STAGE/boot/vmlinuz-lts" "$STICK/atari-os/vmlinuz"
cp "$BUILD/initramfs.zst" "$STICK/atari-os/initramfs"
cp "$BUILD/modloop.sqfs" "$STICK/atari-os/modloop.sqfs"
cat > "$STICK/limine.conf" <<EOF
timeout: 0
quiet: yes
interface_branding: atari-os-x86

/Atari
    protocol: linux
    path: boot():/atari-os/vmlinuz
    module_path: boot():/atari-os/initramfs
    cmdline: $CMDLINE

/Atari (debug: serial shell, verbose)
    protocol: linux
    path: boot():/atari-os/vmlinuz
    module_path: boot():/atari-os/initramfs
    cmdline: loglevel=7 console=ttyS0,115200 drm_kms_helper.fbdev_emulation=0 atari.debug=1
EOF
cat > "$STICK/atari/README.txt" <<'EOF'
atari-os-x86 - this stick boots an Atari 8-bit computer.

  disks/   put .ATR/.XFD/.DCM disk images here
  carts/   put .CAR/.ROM cartridges here
  roms/    optional real Atari OS ROMs (ATARIXL.ROM, ATARIBAS.ROM ...)
  config/  emulator configuration (saved automatically)

Keys: F1 menu, F2 OPTION, F3 SELECT, F4 START, F5 reset (Shift+F5 cold),
      F6 HELP, F7 BREAK, F9 quit (powers off), F11 toggle arrows joystick.
      Arrows + left Alt = joystick 0.  Numpad 8/2/4/6 + 0 = joystick 0.
EOF

# ---- 5. disk image --------------------------------------------------------
log "Creating $OUT (${IMG_MB} MB)"
rm -f "$OUT"
truncate -s "${IMG_MB}M" "$OUT"
# One FAT32 partition (type 0c), bootable, starting at 1 MiB.
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q "$OUT"
PART_OFFSET=$((2048 * 512))
mformat -i "$OUT@@$PART_OFFSET" -F -v ATARI ::
mcopy -i "$OUT@@$PART_OFFSET" -s -Q "$STICK"/* ::
limine bios-install "$OUT" >/dev/null 2>&1
ls -la "$OUT"
log "Done. Boot with: scripts/run-qemu.sh [bios|uefi]"
