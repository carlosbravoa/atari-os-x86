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
# fsck.fat: init checks every FAT before mounting it read-write.
cp "$STAGE/sbin/sfdisk" "$STAGE/sbin/mkfs.fat" "$STAGE/sbin/fsck.fat" "$INITRAMFS/sbin/"
ln -sf mkfs.fat "$INITRAMFS/sbin/mkfs.vfat"
cp /usr/bin/limine "$INITRAMFS/usr/bin/limine"
copy_deps "$INITRAMFS/sbin/sfdisk" "$INITRAMFS/sbin/mkfs.fat" "$INITRAMFS/sbin/fsck.fat" "$INITRAMFS/usr/bin/limine"
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
# Kernel modules old laptops never load (docs/specs/modloop-2.md): paths
# under modules/<kver>/kernel/, a directory or a module file without its
# .ko.gz. Comment a line out (#) to get that class of drivers back. Modules
# that depend on a pruned one go too; the build fails if anything the
# ZenBook, init.c or the keep-list below needs would go.
MODLOOP_PRUNE="
# TV tuners, DVB, capture cards, camera sensors, IR receivers; kept: the
# media core, CEC (the GPU drivers need it) and UVC webcams (the ZenBook
# loads uvcvideo; snd-usb-audio needs mc)
drivers/media/dvb-core drivers/media/dvb-frontends drivers/media/firewire
drivers/media/i2c drivers/media/pci drivers/media/platform drivers/media/rc
drivers/media/spi drivers/media/tuners drivers/media/radio drivers/media/mmc
drivers/media/test-drivers drivers/media/common/b2c2 drivers/media/common/siano
drivers/media/common/cx2341x drivers/media/common/cypress_firmware
drivers/media/common/ttpci-eeprom drivers/media/common/tveeprom
drivers/media/usb/b2c2 drivers/media/usb/cx231xx drivers/media/usb/dvb-usb
drivers/media/usb/dvb-usb-v2 drivers/media/usb/em28xx drivers/media/usb/gspca
drivers/media/usb/s2255 drivers/media/usb/ttusb-budget drivers/media/usb/ttusb-dec
drivers/media/usb/usbtv
# server, cluster and virtualisation host hardware; embedded buses (kept:
# drivers/dca, igb needs it)
drivers/infiniband drivers/md drivers/mtd drivers/iio drivers/xen drivers/vhost
drivers/vfio drivers/vdpa drivers/ntb drivers/rapidio drivers/fpga drivers/greybus
drivers/staging drivers/isdn drivers/atm drivers/uio drivers/accel drivers/crypto
drivers/mailbox drivers/mcb drivers/siox drivers/slimbus drivers/target
drivers/block/drbd drivers/block/nbd drivers/block/rbd drivers/block/pktcdvd
# SAN/RAID host adapters (kept: sd_mod sr_mod sg st ch virtio_scsi, and
# scsi_transport_fc, which Hyper-V's disk driver hv_storvsc needs; ses, the
# enclosure services of disk shelves, goes with scsi_transport_sas)
drivers/scsi/qla2xxx drivers/scsi/qla4xxx drivers/scsi/lpfc drivers/scsi/mpt3sas
drivers/scsi/mpi3mr drivers/scsi/megaraid drivers/scsi/aacraid drivers/scsi/bfa
drivers/scsi/csiostor drivers/scsi/be2iscsi drivers/scsi/fnic drivers/scsi/snic
drivers/scsi/qedf drivers/scsi/qedi drivers/scsi/isci drivers/scsi/pm8001
drivers/scsi/mvsas drivers/scsi/arcmsr drivers/scsi/aic7xxx drivers/scsi/aic94xx
drivers/scsi/smartpqi drivers/scsi/hpsa drivers/scsi/cxgbi drivers/scsi/elx
drivers/scsi/libfc drivers/scsi/fcoe drivers/scsi/libsas drivers/scsi/hisi_sas
drivers/scsi/esas2r drivers/scsi/dpt_i2o drivers/scsi/sym53c8xx_2 drivers/scsi/3w-9xxx
drivers/scsi/3w-sas drivers/scsi/3w-xxxx drivers/scsi/advansys drivers/scsi/hptiop
drivers/scsi/initio drivers/scsi/ipr drivers/scsi/ips drivers/scsi/myrb drivers/scsi/myrs
drivers/scsi/stex drivers/scsi/wd719x drivers/scsi/fdomain drivers/scsi/qlogicfas408
drivers/scsi/qla1280 drivers/scsi/dc395x drivers/scsi/am53c974 drivers/scsi/BusLogic
drivers/scsi/a100u2w drivers/scsi/atp870u drivers/scsi/vmw_pvscsi
drivers/scsi/scsi_transport_sas
drivers/scsi/scsi_transport_iscsi drivers/scsi/iscsi_tcp drivers/scsi/libiscsi
drivers/scsi/libiscsi_tcp
# datacenter NICs (kept: the consumer ethernet, wireless, USB network adapters)
drivers/net/ethernet/mellanox drivers/net/ethernet/intel/ice drivers/net/ethernet/intel/i40e
drivers/net/ethernet/intel/iavf drivers/net/ethernet/intel/fm10k drivers/net/ethernet/intel/ixgbe
drivers/net/ethernet/intel/ixgbevf drivers/net/ethernet/intel/idpf
drivers/net/ethernet/broadcom/bnxt drivers/net/ethernet/broadcom/bnx2x
drivers/net/ethernet/chelsio drivers/net/ethernet/qlogic drivers/net/ethernet/cavium
drivers/net/ethernet/netronome drivers/net/ethernet/emulex drivers/net/ethernet/solarflare
drivers/net/ethernet/huawei drivers/net/ethernet/pensando drivers/net/ethernet/fungible
drivers/net/ethernet/microsoft drivers/net/ethernet/wangxun drivers/net/ethernet/amd/xgbe
drivers/net/ethernet/amd/pds_core drivers/net/ethernet/sfc drivers/net/ethernet/brocade
drivers/net/ethernet/cisco drivers/net/ethernet/myricom drivers/net/ethernet/neterion
drivers/net/ethernet/tehuti drivers/net/ethernet/renesas
drivers/net/fddi drivers/net/hippi drivers/net/wan drivers/net/hamradio drivers/net/ieee802154
drivers/net/can drivers/net/arcnet drivers/net/plip drivers/net/team drivers/net/bonding
drivers/net/ipvlan drivers/net/macvlan drivers/net/ppp drivers/net/slip
# cluster, network and exotic filesystems (kept: ext4 btrfs xfs fat exfat
# ntfs3 isofs udf squashfs fuse overlayfs smb and the rest)
fs/ocfs2 fs/gfs2 fs/dlm fs/ceph fs/nfs fs/nfsd fs/nfs_common fs/lockd fs/afs fs/orangefs
fs/9p fs/coda fs/ubifs fs/jffs2 fs/nilfs2 fs/reiserfs fs/ufs fs/minix fs/bfs fs/qnx4
fs/qnx6 fs/omfs fs/befs fs/adfs fs/affs fs/efs fs/freevxfs fs/sysv fs/hpfs fs/hfs
fs/hfsplus fs/jfs fs/cramfs fs/romfs fs/zonefs fs/bcachefs fs/erofs fs/f2fs
# firewall, traffic shaping and server/cluster network protocols
net/netfilter net/ipv4/netfilter net/ipv6/netfilter net/bridge/netfilter net/sched
net/dccp net/sctp net/rds net/tipc net/batman-adv net/openvswitch net/mpls net/nsh
net/l2tp net/caif net/ceph net/can net/ieee802154 net/6lowpan net/mac802154 net/nfc
net/rxrpc net/smc net/sunrpc
"
# Must survive the prune (section C of the spec): what init.c always loads
# (base_modules[] in init/init.c; keep this in step with it), the
# initramfs's set, and drivers for laptops in the wild. The 182 modules the
# test ZenBook loads are read from docs/specs/zenbook-modules.txt.
MODLOOP_BASE_MODULES="evdev loop squashfs vfat exfat ntfs3 nls_cp437 nls_iso8859-1
	nls_utf8 usb-storage uas sd_mod button af_packet coretemp k10temp"
MODLOOP_KEEP="ath9k ath10k_pci ath11k_pci ath5k rtw88_pci rtw88_8822be rtw88_8822ce
	rtw89_pci rtl8xxxu rtl8192ce rtl8192cu rtl8188ee rtl8723be rtl8821ae rt2800pci
	rt2800usb brcmfmac brcmsmac b43 b43legacy mt76x2e mt7921e mt7925e iwlwifi iwlmvm
	iwldvm iwl3945 iwl4965 ipw2200 r8169 e1000e e1000 igb igc tg3 b44 sky2 skge
	atl1c atl1e atl1 alx forcedeth via_rhine via_velocity jme r8152 asix
	ax88179_178a cdc_ether rndis_host radeon amdgpu nouveau i915 xe
	snd_hda_intel snd_hda_codec_realtek snd_hda_codec_conexant
	snd_hda_codec_hdmi snd_hda_codec_idt snd_hda_codec_sigmatel
	snd_hda_codec_via snd_hda_codec_cirrus snd_sof_pci_intel_tgl
	snd_sof_pci_intel_cnl snd_sof_pci_intel_apl snd_sof_pci_intel_icl
	snd_sof_pci_intel_mtl snd_sof_pci_intel_lnl snd_sof_amd_renoir
	snd_sof_amd_rembrandt snd_acp3x_pdm_dma snd_intel8x0 snd_ens1371
	snd_usb_audio usb_storage uas sdhci sdhci_pci rtsx_pci rtsx_pci_sdmmc
	rtsx_usb rtsx_usb_sdmmc mmc_block memstick thinkpad_acpi asus_wmi
	asus_nb_wmi dell_laptop dell_wmi hp_wmi ideapad_laptop acer_wmi
	samsung_laptop toshiba_acpi sony_laptop fujitsu_laptop msi_wmi
	lg_laptop panasonic_laptop xpad joydev hid_sony hid_microsoft hid_logitech
	hid_logitech_dj hid_nintendo hid_playstation hid_steam hid_multitouch
	i2c_hid i2c_hid_acpi psmouse synaptics_usb elan_i2c atkbd coretemp
	k10temp k8temp fam15h_power acpi_cpufreq intel_pstate
	processor_thermal_device pcc_cpufreq btusb btintel btrtl
	btbcm ath3k usbhid hid_generic evdev uinput loop squashfs vfat
	exfat ntfs3 ext4 btrfs xfs nvme ahci ata_piix pata_acpi nls_cp437
	nls_iso8859_1 nls_utf8 nls_cp850 nls_cp1252 efivarfs efi_pstore
	firewire_ohci firewire_sbp2 ohci_pci ehci_pci xhci_pci uhci_hcd
	pcspkr cdc_acm ftdi_sio pl2303 cp210x ch341 usbserial
	$INITRAMFS_MODULES $LOOPBACK_FS_MODULES"

log "Building modloop (modules + firmware)"
rm -rf "$BUILD/modloop"
mkdir -p "$BUILD/modloop/modules" "$BUILD/modloop/firmware"
cp -a "$STAGE/lib/modules/$KVER" "$BUILD/modloop/modules/"
[ -d "$STAGE/lib/firmware" ] && cp -a "$STAGE/lib/firmware/." "$BUILD/modloop/firmware/"
# Firmware old laptops never load (the stick is 1 GiB, keep the modloop small):
# - Nvidia GSP blobs (RTX 20/30 and later; ~100 MB): nouveau only needs
#   them on Turing+ GPUs, and such laptops drive their panel from the
#   Intel GPU anyway
# - Intel Wi-Fi ucode of older API versions (~50 MB): iwlwifi loads the
#   newest file it finds for a chip, so only the newest per chip stays
#   (the .pnvm files are kept as they are)
rm -rf "$BUILD/modloop/firmware"/nvidia/*/gsp
if [ -d "$BUILD/modloop/firmware/intel/iwlwifi" ]; then
	ls "$BUILD/modloop/firmware/intel/iwlwifi" | sed -n 's/^\(iwlwifi-.*\)-\([0-9][0-9]*\)\.ucode\(\.zst\|\.xz\|\)$/\1 \2 &/p' \
		| sort -k1,1 -k2,2n | awk '{ if ($1 == prev) print last; prev = $1; last = $3 }' \
		| while read -r f; do rm -f "$BUILD/modloop/firmware/intel/iwlwifi/$f"; done
fi
find "$BUILD/modloop/firmware" -type l | while read -r l; do [ -e "$l" ] || rm -f "$l"; done  # dangling symlinks (busybox find has no -xtype)
log "modloop firmware: $(du -sm "$BUILD/modloop/firmware" | cut -f1) MB"
# Prune the modules (MODLOOP_PRUNE above). $STAGE keeps the full set: its
# modules.dep is the reference for what depends on what.
MODS=$BUILD/modloop/modules/$KVER
ODEP=$STAGE/lib/modules/$KVER/modules.dep
mods_mb() { du -sm "$MODS" | cut -f1; }
mods_count() { find "$MODS/kernel" -name '*.ko*' | wc -l; }
before_mb=$(mods_mb); before_n=$(mods_count)
absent=""
for p in $(printf '%s\n' "$MODLOOP_PRUNE" | sed 's/#.*//'); do
	found=0
	[ -d "$MODS/kernel/$p" ] && { rm -rf "$MODS/kernel/$p"; found=1; }
	for f in "$MODS/kernel/$p".ko*; do [ -f "$f" ] && { rm -f "$f"; found=1; }; done
	[ $found = 1 ] || absent="$absent $p"
done
[ -n "$absent" ] && log "modloop prune: not in this kernel:$absent"
# Modules that need a pruned one (per the original modules.dep) go too,
# until nothing is left that depends on a missing module.
(cd "$MODS" && find kernel -name '*.ko*') > "$BUILD/modloop-present"
awk -F': *' 'NR == FNR { have[$0] = 1; next }
	$1 in have { mods[$1] = $2 }
	END {
		do {
			changed = 0
			for (m in mods) {
				if (!(m in have)) continue
				n = split(mods[m], d, " ")
				for (i = 1; i <= n; i++) if (!(d[i] in have)) { delete have[m]; print m; changed = 1; break }
			}
		} while (changed)
	}' "$BUILD/modloop-present" "$ODEP" > "$BUILD/modloop-orphans"
(cd "$MODS" && xargs -r rm -f < "$BUILD/modloop-orphans")
[ -s "$BUILD/modloop-orphans" ] && log "modloop prune: $(wc -l < "$BUILD/modloop-orphans") modules depending on pruned ones removed too:" \
	"$(sed 's|.*/||; s|\.ko.*||' "$BUILD/modloop-orphans" | sort | tr '\n' ' ')"
find "$MODS/kernel" -depth -type d -exec rmdir {} + 2>/dev/null || true  # the emptied ones
# Regenerate modules.dep/alias/symbols/... for the pruned tree (depmod wants
# <base>/lib/modules/<kver>, so the tree is moved under a temp base for it).
modname() { sed 's|.*/||; s|\.ko.*||; s|-|_|g'; }
(cd "$MODS" && find kernel -name '*.ko*' | sed 's|\.ko\..*|.ko|') | sort > "$BUILD/modloop-present"
awk 'NR == FNR { have[$0] = 1; next } $0 in have' "$BUILD/modloop-present" \
	"$STAGE/lib/modules/$KVER/modules.order" > "$MODS/modules.order"
rm -f "$MODS"/modules.*.bin
rm -rf "$BUILD/depmod-base"
mkdir -p "$BUILD/depmod-base/lib/modules"
mv "$MODS" "$BUILD/depmod-base/lib/modules/$KVER"
# (its complaint about the vmlinuz symlink, dangling here, is filtered out)
depmod -b "$BUILD/depmod-base" "$KVER" 2> "$BUILD/depmod.err" \
	|| { cat "$BUILD/depmod.err" >&2; mv "$BUILD/depmod-base/lib/modules/$KVER" "$MODS"; die "depmod failed"; }
grep -v 'fstatat(.*vmlinuz)' "$BUILD/depmod.err" >&2 || true
rm -f "$BUILD/depmod.err"
mv "$BUILD/depmod-base/lib/modules/$KVER" "$MODS"
rm -rf "$BUILD/depmod-base"
# Safety check (spec section C): the ZenBook's modules, init.c's
# base_modules[] and the keep-list must still be there, each with all its
# dependencies. Names compare with '-' and '_' as equal (like modprobe).
# Every remaining module must keep exactly its old dependencies (a module
# that lost one would fail with "Unknown symbol").
modloop_names() { sed 's|^\([^:]*\):.*|\1|' "$1" | modname | sort -u; }
modloop_names "$ODEP" > "$BUILD/modloop-names-before"
modloop_names "$MODS/modules.dep" > "$BUILD/modloop-names-after"
fail=""; unknown=""
check_names() { # strict: 1 = must exist even before the prune
	strict=$1; shift
	for m in $(printf '%s\n' "$@" | modname); do
		grep -qx "$m" "$BUILD/modloop-names-after" && continue
		if grep -qx "$m" "$BUILD/modloop-names-before" || [ "$strict" = 1 ]; then
			fail="$fail $m"
		else
			unknown="$unknown $m"
		fi
	done
}
[ -f "$WORK/docs/specs/zenbook-modules.txt" ] || die "docs/specs/zenbook-modules.txt missing"
check_names 1 $(cat "$WORK/docs/specs/zenbook-modules.txt")
check_names 1 $MODLOOP_BASE_MODULES
check_names 0 $MODLOOP_KEEP
[ -n "$unknown" ] && log "modloop keep-list: not in this kernel (built in or not configured):$(printf '%s\n' $unknown | sort -u | tr '\n' ' ')"
[ -z "$fail" ] || die "modloop prune removes modules that must stay:$(printf '%s\n' $fail | sort -u | tr '\n' ' ')"
awk -F': *' '{ n = split($2, d, " "); for (i = 1; i <= n; i++) print d[i] }' "$MODS/modules.dep" | sort -u \
	| while read -r d; do [ -f "$MODS/$d" ] || echo "$d"; done > "$BUILD/modloop-missing"
[ -s "$BUILD/modloop-missing" ] && die "modloop modules.dep lists missing files: $(tr '\n' ' ' < "$BUILD/modloop-missing")"
deppairs() { awk -F': *' '{ print $1; n = split($2, d, " "); for (i = 1; i <= n; i++) print $1 " " d[i] }' "$1" | sort; }
deppairs "$MODS/modules.dep" > "$BUILD/modloop-deps-after"
deppairs "$ODEP" | awk 'NR == FNR { if (!index($0, " ")) have[$0] = 1; next } $1 in have' "$BUILD/modloop-deps-after" - > "$BUILD/modloop-deps-before"
cmp -s "$BUILD/modloop-deps-before" "$BUILD/modloop-deps-after" \
	|| die "modloop prune changed module dependencies: $(diff "$BUILD/modloop-deps-before" "$BUILD/modloop-deps-after" | head -20)"
rm -f "$BUILD"/modloop-present "$BUILD"/modloop-orphans "$BUILD"/modloop-names-* "$BUILD"/modloop-missing "$BUILD"/modloop-deps-*
log "modloop modules: $before_mb -> $(mods_mb) MB ($before_n -> $(mods_count) modules)"
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
