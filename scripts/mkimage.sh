#!/bin/sh
# mkimage.sh - assemble the bootable atari-os-x86 image (run inside the Alpine env)
#
#   scripts/alpine-env.sh run scripts/mkimage.sh
#
# Produces build/atari-os-x86-<version>.img (and build/atari-os.img, a hard
# link to it, for the local scripts): an MBR disk with one FAT32 partition
# that boots on BIOS and UEFI through Limine, plus
# build/atari-os-x86-<version>-files.zip, the same files for the dual-boot
# installers (scripts/install-linux.sh, scripts/install-windows.ps1).
# Layout of the stick:
#
#   /EFI/BOOT/BOOTX64.EFI     UEFI loader (Limine)
#   /limine-bios.sys          BIOS loader stage (plus code in the MBR)
#   /limine.conf
#   /atari-os/vmlinuz         Alpine linux-lts
#   /atari-os/initramfs       our init + emulator + minimal driver set
#   /atari-os/modloop.sqfs    full module set + firmware (squashfs)
#   /atari-os/VERSION         the version string (also /etc/atari-os-release)
#   /atari/                   the user's area: disks, carts, roms, config
#
# Everything above /atari is replaceable by copying a new build over it.
#
# Environment: ATARI_OS_VERSION overrides the version (default: git describe
# --tags --always, plus -dirty for uncommitted changes in this repo; the
# build's own patches inside the submodules do not count), IMG_MB the image
# size, EXTRA_CMDLINE extra kernel options, SKIP_STAGE=1 reuses the
# unpacked packages, DEV_SSH_KEY=0 leaves out the developer key (release
# builds: an image that carries authorized_keys never makes the stick
# holder's key), KEEP_OLD_IMAGES=1 keeps older build/atari-os-x86-*.img.

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f /etc/alpine-release ] || { echo "error: run me through scripts/alpine-env.sh run $0 (or in an Alpine container)" >&2; exit 1; }
WORK=$PROJECT
BUILD=$WORK/build
STAGE=$BUILD/stage          # packages unpacked here
INITRAMFS=$BUILD/initramfs  # initramfs tree
IMG_MB=${IMG_MB:-1024}

# fbdev_emulation=0: no fbcon on the panel, ever. The emulator owns the
# display; a late fbcon takeover otherwise commits over its scanout.
# Power: Intel panel self refresh and framebuffer compression (default on
# in recent kernels; explicit here), no NMI watchdog (a timer per CPU).
CMDLINE="quiet loglevel=3 drm_kms_helper.fbdev_emulation=0 vt.global_cursor_default=0 i915.enable_psr=1 i915.enable_fbc=1 nmi_watchdog=0 ${EXTRA_CMDLINE:-}"

# Firmware: GPUs, Intel Wi-Fi (linux-firmware-intel carries iwlwifi), the
# SOF audio DSP on 2019+ Intel laptops, and the wireless regulatory db.
FIRMWARE_PKGS="linux-firmware-i915 linux-firmware-radeon linux-firmware-nvidia linux-firmware-amdgpu
	linux-firmware-intel sof-firmware wireless-regdb"

# Modules that must be inside the initramfs: enough to see the stick and
# the keyboard before the modloop is available. Dependencies are resolved
# (modules.dep), soft dependencies are not.
INITRAMFS_MODULES="evdev usbhid hid-generic xhci-pci xhci-hcd ehci-pci ehci-hcd
	ohci-pci ohci-hcd uhci-hcd usb-storage uas sd_mod ahci nvme mmc_block sdhci-pci
	loop squashfs vfat nls_cp437 nls_iso8859-1 nls_utf8 exfat ntfs3 button
	virtio_pci virtio_blk virtio_input virtio_scsi bochs af_packet"
# The host filesystems of the loopback install (the image as a file on a
# Linux partition: atari.image=, scripts/install-linux.sh --image), with
# btrfs's checksum drivers (soft dependencies). They are big (3.6 MB as
# .ko.gz), so in the initramfs they are recompressed as .ko.xz (2.4 MB;
# busybox modprobe unpacks xz like gzip); the modloop keeps the originals.
LOOPBACK_FS_MODULES="ext4 xfs btrfs crc32c-cryptoapi xxhash_generic blake2b_generic"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 0. version -----------------------------------------------------------
if [ -z "${ATARI_OS_VERSION:-}" ]; then
	if ATARI_OS_VERSION=$(git -C "$WORK" describe --tags --always 2>/dev/null); then
		# Like --dirty, but the patches and symlinks build-emulator.sh and
		# build-fujinet.sh put into the submodules are not "dirty".
		git -C "$WORK" update-index -q --refresh >/dev/null 2>&1 || true
		git -C "$WORK" diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null \
			|| ATARI_OS_VERSION="$ATARI_OS_VERSION-dirty"
	else
		ATARI_OS_VERSION=unknown
	fi
fi
VERSION=$ATARI_OS_VERSION
# It ends up in a file name, on the splash and in the menus.
case "$VERSION" in
	*[!A-Za-z0-9._+-]* | "" | -* | .*) die "bad version string '$VERSION' (letters, digits, . _ + - only)" ;;
esac
[ ${#VERSION} -le 40 ] || die "version string too long: $VERSION"
OUT=$BUILD/atari-os-x86-$VERSION.img
ZIP=$BUILD/atari-os-x86-$VERSION-files.zip   # the stick's files, for the dual-boot scripts
LINK=$BUILD/atari-os.img    # stable name for run-qemu.sh, qemu-test.sh, mk-ventoy-test.sh
log "Version $VERSION"

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
	expat libcrypto3 zlib libstdc++ libgcc ca-certificates-bundle \
	wpa_supplicant iw dropbear dropbear-convert sfdisk dosfstools $FIRMWARE_PKGS >/dev/null
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

gcc -static -Os -Wall -Wextra -I"$WORK/common" -o "$INITRAMFS/init" "$WORK/init/init.c"
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

# Shared libraries: resolve NEEDED entries from the stage recursively.
copy_deps() {
	for bin in "$@"; do
		readelf -d "$bin" 2>/dev/null | awk '/NEEDED/ {gsub(/[][]/,"",$5); print $5}' | while read -r lib; do
			[ -e "$INITRAMFS/usr/lib/$lib" ] || [ -e "$INITRAMFS/lib/$lib" ] && continue
			src=""
			for d in "$STAGE/usr/lib" "$STAGE/lib"; do [ -e "$d/$lib" ] && src="$d/$lib" && break; done
			[ -n "$src" ] || { echo "warning: $lib for $bin not found in stage" >&2; continue; }
			cp "$src" "$INITRAMFS/usr/lib/$lib"
			copy_deps "$src"
		done
	done
}

# FujiNet-PC: binary, web UI data, default config; plus its shared libraries.
if [ -x "$BUILD/fujinet/dist/fujinet" ]; then
	mkdir -p "$INITRAMFS/usr/lib/fujinet" "$INITRAMFS/etc/ssl/certs"
	cp "$BUILD/fujinet/dist/fujinet" "$INITRAMFS/usr/lib/fujinet/"
	strip "$INITRAMFS/usr/lib/fujinet/fujinet"
	cp -r "$BUILD/fujinet/dist/data" "$INITRAMFS/usr/lib/fujinet/"
	# Empty SSID: CONFIG must not wait on a fake "Dummy Cafe" network; init drives Wi-Fi.
	sed "s/^SSID=.*/SSID=/" "$BUILD/fujinet/dist/fnconfig.ini" > "$INITRAMFS/usr/lib/fujinet/fnconfig.ini"
	copy_deps "$INITRAMFS/usr/lib/fujinet/fujinet"
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
		# one-shot network time, retried in the background until it works;
		# the RTC is written back so the next boot starts right
		[ -e /run/ntp-done ] || (for i in 1 2 3 4 5 6; do busybox ntpd -q -n -p pool.ntp.org >/dev/null 2>&1 && { busybox hwclock -w -u; : > /run/ntp-done; echo "ntp: clock set" > /dev/kmsg; break; }; sleep 20; done) &
		;;
esac
exit 0
EOS
chmod +x "$INITRAMFS/etc/udhcpc.script"
# Never bind the PC-speaker sound driver; it would show up as a sound card.
mkdir -p "$INITRAMFS/etc/modprobe.d"
printf 'blacklist snd_pcsp\nblacklist pcspkr\n' > "$INITRAMFS/etc/modprobe.d/blacklist.conf"
# Intel Wi-Fi: power scheme 1 = always on. Balanced mode adds 1-2 s of
# latency to inbound packets on an idle link, which makes SSH and FujiNet
# feel broken.
printf 'options iwlmvm power_scheme=1\noptions iwlwifi power_save=0\n' > "$INITRAMFS/etc/modprobe.d/iwlwifi.conf"
echo "atari" > "$INITRAMFS/etc/hostname"
echo "$VERSION" > "$INITRAMFS/etc/atari-os-release"
printf '127.0.0.1 localhost atari\n' > "$INITRAMFS/etc/hosts"

copy_deps "$INITRAMFS/usr/bin/atari800"

# Wi-Fi and SSH userspace (driven by init and the F8 System menu).
mkdir -p "$INITRAMFS/sbin" "$INITRAMFS/usr/sbin" "$INITRAMFS/root/.ssh" "$INITRAMFS/etc/dropbear"
cp "$STAGE/sbin/wpa_supplicant" "$STAGE/sbin/wpa_cli" "$INITRAMFS/sbin/"
cp "$STAGE/usr/sbin/iw" "$INITRAMFS/usr/sbin/"
cp "$STAGE/usr/sbin/dropbear" "$INITRAMFS/usr/sbin/"
cp "$STAGE/usr/bin/dropbearkey" "$STAGE/usr/bin/dropbearconvert" "$INITRAMFS/usr/bin/"
copy_deps "$INITRAMFS/sbin/wpa_supplicant" "$INITRAMFS/sbin/wpa_cli" "$INITRAMFS/usr/sbin/iw" "$INITRAMFS/usr/sbin/dropbear" \
	"$INITRAMFS/usr/bin/dropbearkey" "$INITRAMFS/usr/bin/dropbearconvert"
# The installer (F8 "Install onto this laptop's disk"): sfdisk, mkfs.vfat
# and the limine CLI for the MBR. limine comes from this build environment,
# not the stage: it is the same package whose limine-bios.sys and
# BOOTX64.EFI go onto the stick (step 4), and bios-install must match them.
cp "$STAGE/sbin/sfdisk" "$STAGE/sbin/mkfs.fat" "$INITRAMFS/sbin/"
ln -sf mkfs.fat "$INITRAMFS/sbin/mkfs.vfat"
cp /usr/bin/limine "$INITRAMFS/usr/bin/limine"
copy_deps "$INITRAMFS/sbin/sfdisk" "$INITRAMFS/sbin/mkfs.fat" "$INITRAMFS/usr/bin/limine"
# dropbear needs an account database for root
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$INITRAMFS/etc/passwd"
printf 'root:x:0:\n' > "$INITRAMFS/etc/group"
printf 'root:!::0:::::\n' > "$INITRAMFS/etc/shadow"
chmod 600 "$INITRAMFS/etc/shadow"
printf 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin\nexport HOME=/root\n' > "$INITRAMFS/etc/profile"

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
resolve_mods $INITRAMFS_MODULES $LOOPBACK_FS_MODULES | while read -r rel; do
	mkdir -p "$INITRAMFS/lib/modules/$KVER/$(dirname "$rel")"
	cp "$MODDIR/$rel" "$INITRAMFS/lib/modules/$KVER/$rel"
done
# shellcheck disable=SC2086
resolve_mods $LOOPBACK_FS_MODULES | while read -r rel; do
	case "$rel" in *.ko.gz) ;; *) continue ;; esac
	f=$INITRAMFS/lib/modules/$KVER/$rel
	gzip -dc "$f" | xz -9e --check=crc32 -c > "${f%.gz}.xz"
	rm "$f"
	sed -i "s|^$rel:|${rel%.gz}.xz:|; s| $rel\$| ${rel%.gz}.xz|; s| $rel | ${rel%.gz}.xz |g" \
		"$INITRAMFS/lib/modules/$KVER/modules.dep"
done
rm -f "$INITRAMFS/lib/modules/$KVER/modules.dep.bin" # stale now; busybox reads modules.dep
log "initramfs modules: $(find "$INITRAMFS/lib/modules" -name '*.ko*' | wc -l)," \
	"loopback filesystems $(find "$INITRAMFS/lib/modules" -name '*.ko.xz' -exec cat {} + | wc -c) bytes (xz)"

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
	"$STICK/atari/roms" "$STICK/atari/config" "$STICK/atari/library"
cp /usr/share/limine/BOOTX64.EFI "$STICK/EFI/BOOT/"
cp /usr/share/limine/limine-bios.sys "$STICK/"
cp "$STAGE/boot/vmlinuz-lts" "$STICK/atari-os/vmlinuz"
cp "$BUILD/initramfs.zst" "$STICK/atari-os/initramfs"
cp "$BUILD/modloop.sqfs" "$STICK/atari-os/modloop.sqfs"
echo "$VERSION" > "$STICK/atari-os/VERSION"
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
# A developer key, generated once under build/ssh/, is authorised on every
# image built here, so the builder can reach a laptop as soon as SSH is
# switched on in the F8 menu (the laptop still generates its own key for
# the stick holder when this file is absent).
if [ "${DEV_SSH_KEY:-1}" = 0 ]; then
	log "No developer SSH key (DEV_SSH_KEY=0)"
elif [ ! -f "$BUILD/ssh/id_ed25519" ]; then
	mkdir -p "$BUILD/ssh"
	dropbearkey -t ed25519 -f "$BUILD/ssh/id_ed25519.dropbear" >/dev/null 2>&1 \
		&& dropbearconvert dropbear openssh "$BUILD/ssh/id_ed25519.dropbear" "$BUILD/ssh/id_ed25519" >/dev/null 2>&1 \
		&& dropbearkey -y -f "$BUILD/ssh/id_ed25519.dropbear" 2>/dev/null | grep '^ssh-ed25519' > "$BUILD/ssh/id_ed25519.pub" \
		&& chmod 600 "$BUILD/ssh/id_ed25519" || rm -f "$BUILD/ssh/id_ed25519"
fi
if [ "${DEV_SSH_KEY:-1}" != 0 ] && [ -f "$BUILD/ssh/id_ed25519.pub" ]; then
	mkdir -p "$STICK/atari/ssh"
	cp "$BUILD/ssh/id_ed25519.pub" "$STICK/atari/ssh/authorized_keys"
	log "Developer SSH key authorised (build/ssh/id_ed25519)"
fi

# ASH, the Atari shell (third_party/atari800-atari-shell, prebuilt): the
# cartridge for boot=ash, the program and the boot disk; atari/hdd is its h:
# device (the emulator's SIO device $72). README.TXT uses ATASCII line ends.
ASH_REL=$WORK/third_party/atari800-atari-shell/release
if [ -f "$ASH_REL/ASH.CAR" ]; then
	mkdir -p "$STICK/atari/ash" "$STICK/atari/hdd"
	cp "$ASH_REL/ASH.CAR" "$STICK/atari/ash/ASH.CAR"
	cp "$ASH_REL/ASH-TN.XEX" "$STICK/atari/ash/ASH.XEX"
	cp "$ASH_REL/ASH-TN.ATR" "$STICK/atari/ash/ASH.ATR"
	printf '%s\n' \
		"h: is this folder, atari/hdd on" \
		"the stick: files for the Atari, 8.3" \
		"names, no subfolders." \
		"" \
		"sd: is the stick and USB drives:" \
		"  sd:/atari  the stick's atari folder" \
		"  sd:/usb    one folder per USB drive" \
		"Writing to sd: needs 'sd rw' first." \
		"atari/ash and atari/config stay" \
		"read-only." \
		| tr '\n' '\233' > "$STICK/atari/hdd/README.TXT"
else
	log "ASH not found (git submodule update --init third_party/atari800-atari-shell)"
fi

# FujiNet's SD card (fujinet-pc -s atari/fujinet/SD; CONFIG's host "SD"),
# with a note in ATASCII (EOL 155) so it reads right on the Atari too.
mkdir -p "$STICK/atari/fujinet/SD"
printf '%s\n' \
	"This is FujiNet's SD card." \
	"" \
	"Put ATR/XEX files here from the" \
	"Library (F9, a file, Copy to FujiNet" \
	"SD) or over SSH/USB: it is the" \
	"atari/fujinet/SD folder on the stick." \
	"" \
	"CONFIG lists it as host SD." \
	| tr '\n' '\233' > "$STICK/atari/fujinet/SD/README.TXT"

cat > "$STICK/atari/README.txt" <<'EOF'
atari-os-x86 - this stick boots an Atari 8-bit computer.

  library/ your collection: any folders, any Atari files (F9 = Library)
  disks/   put .ATR/.XFD/.DCM disk images here
  carts/   put .CAR/.ROM cartridges here
  roms/    optional real Atari OS ROMs (ATARIXL.ROM, ATARIBAS.ROM ...)
  ash/     ASH, the Atari shell (F8 > Boot into: ASH)
  hdd/     ASH's h: drive
  config/  emulator configuration (saved automatically)

Keys: F1 menu, F2 OPTION, F3 SELECT, F4 START, F5 reset (Shift+F5 cold),
      F6 HELP, F7 BREAK, F8 System menu (shut down there), F9 Library,
      F11 toggle arrows joystick.
      Arrows + left Alt = joystick 0.  Numpad 8/2/4/6 + 0 = joystick 0.
EOF

# ---- 5. disk image --------------------------------------------------------
log "Creating $OUT (${IMG_MB} MB)"
rm -f "$OUT" "$LINK" "$ZIP"
if [ "${KEEP_OLD_IMAGES:-0}" != 1 ]; then
	for old in "$BUILD"/atari-os-x86-*.img "$BUILD"/atari-os-x86-*-files.zip; do
		[ -f "$old" ] || continue
		log "Removing older $(basename "$old")"
		rm -f "$old"
	done
fi
truncate -s "${IMG_MB}M" "$OUT"
# One FAT32 partition (type 0c), bootable, starting at 1 MiB.
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q "$OUT"
PART_OFFSET=$((2048 * 512))
mformat -i "$OUT@@$PART_OFFSET" -F -v ATARI ::
mcopy -i "$OUT@@$PART_OFFSET" -s -Q "$STICK"/* ::
limine bios-install "$OUT" >/dev/null 2>&1
# A hard link, not a copy: no second GiB on disk; whatever writes to one
# (run-qemu.sh boots without -snapshot) changes both.
ln -f "$OUT" "$LINK"
ls -la "$OUT" "$LINK"

# ---- 6. the same files as a zip (dual boot: install-linux.sh --partition,
# install-windows.ps1): the stick's FAT content, nothing else.
log "Creating $(basename "$ZIP")"
(cd "$STICK" && zip -r -X -q "$ZIP" .)
ls -la "$ZIP"
log "Done. $(basename "$OUT") ($VERSION). Boot with: scripts/run-qemu.sh [bios|uefi]"
