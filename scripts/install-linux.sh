#!/bin/sh
# install-linux.sh - boot atari-os-x86 from GRUB next to an existing Linux
# (dual boot), without touching the Linux installation's own boot setup.
#
#   sudo sh install-linux.sh --partition /dev/sdXN atari-os-x86-<v>-files.zip
#        partition mode: a FAT32 partition of 1.5 GiB or more holds the files
#   sudo sh install-linux.sh --image /path/on/a/linux/fs atari-os-x86-<v>.img[.xz]
#        loopback mode: the image as a file on an ext4, btrfs or xfs
#        partition, no partitioning at all
#   sudo sh install-linux.sh --remove      drop the GRUB entry (the files stay)
#   sh install-linux.sh --check            what the script sees; changes nothing
#
# Options: --dry-run (print the plan, write nothing), --format (partition
# mode: make the partition FAT32 first; ERASES it), --force (loopback mode:
# replace an existing image file), --name "Atari" (the menu title), --help.
#
# The GRUB menu entry goes into custom.cfg next to grub.cfg, which every
# grub.cfg made by grub-mkconfig reads through /etc/grub.d/41_custom. The
# script never edits grub.cfg or /etc/default/grub, never runs update-grub,
# grub-install or efibootmgr, never changes the default entry. It writes
# custom.cfg only when there is none or the existing one is its own (first
# line is the marker below); --remove deletes only its own custom.cfg.
# Partitions are never picked for you, and every write to one comes after
# you type its device name and "yes".
#
# Test hook (not for users): --root DIR takes /boot, /etc and
# /sys/firmware from DIR instead of / and skips the root check; block
# devices, mounts and the image path are the real ones.
#
# POSIX sh (dash, busybox ash, bash). Licence: GPL-2.0-or-later, like the
# rest of atari-os-x86.

set -eu

MARKER='# atari-os-x86 (install-linux.sh; remove with --remove)'
MENU_ID=atari-os
MIN_PART_BYTES=$((1536 * 1024 * 1024))   # 1.5 GiB
SPARE_BYTES=$((64 * 1024 * 1024))
ESP_GUID=c12a7328-f81f-11d2-ba4b-00a0c93ec93b
SB_VAR=SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c
# Kernel options when the image's limine.conf cannot be read (the same as
# scripts/mkimage.sh writes into the first entry).
DEFAULT_CMDLINE='quiet loglevel=3 drm_kms_helper.fbdev_emulation=0 vt.global_cursor_default=0 i915.enable_psr=0 i915.enable_fbc=1 nmi_watchdog=0 usbhid.jspoll=1 usbhid.kbpoll=1 usbhid.mousepoll=1'

if [ -t 2 ]; then
	RED=$(printf '\033[1;31m') BLUE=$(printf '\033[1;34m') YEL=$(printf '\033[1;33m') OFF=$(printf '\033[0m')
else
	RED='' BLUE='' YEL='' OFF=''
fi
say() { printf '%s==>%s %s\n' "$BLUE" "$OFF" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$YEL" "$OFF" "$*" >&2; }
die() { printf '%serror:%s %s\n' "$RED" "$OFF" "$*" >&2; exit 1; }

usage() {
	cat <<'EOF'
Usage:
  sudo sh install-linux.sh --partition /dev/sdXN atari-os-x86-<v>-files.zip
  sudo sh install-linux.sh --image /path/on/linux/fs atari-os-x86-<v>.img[.xz]
  sudo sh install-linux.sh --remove
  sh install-linux.sh --check

Adds "Atari" to this computer's GRUB menu, so atari-os-x86 boots next to
the Linux that is installed here. Two ways:

  --partition DEV   a FAT32 partition of 1.5 GiB or more (an empty one, or
                    an earlier Atari install) gets the files from the zip.
                    It must not be mounted, in /etc/fstab, the EFI system
                    partition, swap, LVM, RAID or encrypted. With --format
                    any unused partition is made FAT32 first (ERASED).
  --image PATH      the .img (or .img.xz) is copied to PATH, a new file on
                    an ext4, btrfs or xfs filesystem with room for it (a
                    directory: the file goes into it). Nothing is
                    partitioned; the Atari's files live inside the image
                    and the Atari writes to them there.

  --remove          delete the GRUB entry (custom.cfg, only if it is ours);
                    the partition or image file stays: delete it yourself
  --check           show GRUB, Secure Boot and the partitions, change nothing
  --dry-run         print the plan (and why a target is refused), write nothing
  --format          partition mode: mkfs.vfat the partition first
  --force           loopback mode: replace an existing file at PATH (the
                    Atari files inside the old image are lost)
  --name TITLE      menu title (default "Atari")
  -h, --help        this text

The entry is written to custom.cfg next to grub.cfg (GRUB's 41_custom reads
it). grub.cfg, /etc/default/grub and the default entry are never changed and
update-grub is never run. Secure Boot must be off (Limine and the Atari's
kernel are unsigned). Before anything is written to a partition you type its
device name and "yes".
EOF
}

# ---- arguments ------------------------------------------------------------

MODE='' TARGET='' SOURCE='' DRY=0 FORMAT=0 FORCE=0 NAME=Atari ROOT=''
set_mode() { [ -z "$MODE" ] || [ "$MODE" = "$1" ] || die "--partition, --image, --remove and --check exclude each other"; MODE=$1; }
while [ $# -gt 0 ]; do
	case "$1" in
		--partition) [ $# -ge 2 ] || die "--partition needs a device (/dev/sdXN)"; set_mode partition; TARGET=$2; shift ;;
		--partition=*) set_mode partition; TARGET=${1#--partition=} ;;
		--image) [ $# -ge 2 ] || die "--image needs a path"; set_mode image; TARGET=$2; shift ;;
		--image=*) set_mode image; TARGET=${1#--image=} ;;
		--remove) set_mode remove ;;
		--check) set_mode check ;;
		--dry-run) DRY=1 ;;
		--format) FORMAT=1 ;;
		--force) FORCE=1 ;;
		--name) [ $# -ge 2 ] || die "--name needs a title"; NAME=$2; shift ;;
		--name=*) NAME=${1#--name=} ;;
		--root) [ $# -ge 2 ] || die "--root needs a directory"; ROOT=$2; shift ;;
		--root=*) ROOT=${1#--root=} ;;
		-h|--help) usage; exit 0 ;;
		--) shift; break ;;
		-*) die "unknown option $1 (see --help)" ;;
		*) [ -z "$SOURCE" ] || die "only one zip or image, please (see --help)"; SOURCE=$1 ;;
	esac
	shift
done
if [ $# -gt 0 ]; then
	[ -z "$SOURCE" ] && [ $# -eq 1 ] || die "only one zip or image, please (see --help)"
	SOURCE=$1
fi
[ -n "$MODE" ] || { usage >&2; exit 2; }
case "$MODE" in
	partition|image) [ -n "$SOURCE" ] || die "which file? (see --help)" ;;
	*) [ -z "$SOURCE" ] || die "--$MODE takes no file" ;;
esac
[ "$FORMAT" = 0 ] || [ "$MODE" = partition ] || die "--format is for --partition only"
[ "$FORCE" = 0 ] || [ "$MODE" = image ] || die "--force is for --image only"
case "$NAME" in
	''|*[\"\\\$\`]*) die "--name: give a title without quotes, backslashes, \$ or backticks" ;;
esac
case "$NAME" in *"
"*) die "--name: one line only" ;; esac
if [ -n "$ROOT" ]; then
	[ -d "$ROOT" ] || die "--root: no directory $ROOT"
	ROOT=$(cd "$ROOT" && pwd)
fi

IS_ROOT=0
[ "$(id -u)" = 0 ] && IS_ROOT=1
# Writing needs root; --check and --dry-run show what they can without it.
if [ "$IS_ROOT" = 0 ] && [ -z "$ROOT" ] && [ "$MODE" != check ] && [ "$DRY" = 0 ]; then
	die "run with sudo (sudo sh $0 ...); --check and --dry-run work without"
fi

# ---- helpers --------------------------------------------------------------

have() { command -v "$1" >/dev/null 2>&1; }
file_size() { stat -c %s "$1" 2>/dev/null || wc -c < "$1" | tr -d ' '; }
human() {
	awk -v b="$1" 'BEGIN { split("B KiB MiB GiB TiB", u, " "); i = 1
		while (b >= 1024 && i < 5) { b /= 1024; i++ }
		printf (i == 1) ? "%d %s" : "%.1f %s", b, u[i] }'
}
# Tools the mode needs; one line naming the packages that bring them.
need_tools() {
	nt_missing=''
	for nt_t in "$@"; do have "$nt_t" || nt_missing="$nt_missing $nt_t"; done
	[ -z "$nt_missing" ] && return 0
	nt_pkgs=''
	for nt_t in $nt_missing; do
		case "$nt_t" in
			mkfs.vfat) nt_pkgs="$nt_pkgs dosfstools" ;;
			unzip) nt_pkgs="$nt_pkgs unzip" ;;
			xz) nt_pkgs="$nt_pkgs xz-utils(Debian/Ubuntu)/xz(Fedora/Arch)" ;;
			chattr) nt_pkgs="$nt_pkgs e2fsprogs" ;;
			*) nt_pkgs="$nt_pkgs util-linux" ;;
		esac
	done
	# shellcheck disable=SC2086 # one package per word
	die "missing tools:$nt_missing (install:$(printf '%s\n' $nt_pkgs | sort -u | tr '\n' ' ' | sed 's/ $//; s/^/ /'))"
}
confirm_yes() {
	printf '%s' "$1" >&2
	read -r cy_answer || cy_answer=''
	[ "$cy_answer" = yes ] || { echo "Cancelled, nothing written." >&2; exit 1; }
}
# Value of KEY=... in /etc/default/grub (and /etc/default/grub.d/*.cfg,
# which Ubuntu reads after it), quotes removed; the last one wins.
grub_default() {
	for gd_f in "$ROOT/etc/default/grub" "$ROOT"/etc/default/grub.d/*.cfg; do
		[ -f "$gd_f" ] && sed -n "s/^[[:space:]]*$1=//p" "$gd_f"
	done 2>/dev/null | tail -n 1 | sed "s/^[\"']//; s/[\"'][[:space:]]*\$//"
}

# ---- GRUB: which grub.cfg, and does it read custom.cfg ----------------------

GRUB_CFG='' GRUB_SEEN='' GRUB_GUESS=0
find_grub_cfg() {
	GRUB_CFG='' GRUB_SEEN='' GRUB_GUESS=0
	for fg_f in "$ROOT/boot/grub/grub.cfg" "$ROOT/boot/grub2/grub.cfg" "$ROOT"/boot/efi/EFI/*/grub.cfg; do
		[ -f "$fg_f" ] || continue
		GRUB_SEEN="$GRUB_SEEN $fg_f"
		# Not readable without root (Ubuntu: mode 600). For --check and
		# --dry-run: an enabled 41_custom almost certainly put the
		# custom.cfg lines into it.
		if [ ! -r "$fg_f" ]; then
			if [ -x "$ROOT/etc/grub.d/41_custom" ]; then
				GRUB_CFG=$fg_f GRUB_GUESS=1
				return 0
			fi
			continue
		fi
		# An EFI stub grub.cfg (Fedora, Ubuntu) only configfile's the real
		# one under /boot, which was tried first: it does not count.
		if grep -q 'custom\.cfg' "$fg_f" 2>/dev/null; then
			GRUB_CFG=$fg_f
			return 0
		fi
	done
	return 1
}
grub_flavour() {
	for gf_t in grub-install grub2-install; do
		if have "$gf_t"; then "$gf_t" --version 2>/dev/null | head -n 1; return; fi
	done
	echo "unknown (no grub-install on PATH)"
}
grub_reboot_cmd() { if have grub2-reboot && ! have grub-reboot; then echo grub2-reboot; else echo grub-reboot; fi; }
# none | ours | foreign, for the custom.cfg in directory $1
custom_state() {
	if [ ! -e "$1/custom.cfg" ]; then echo none
	elif [ ! -r "$1/custom.cfg" ]; then echo unreadable
	elif [ "$(head -n 1 "$1/custom.cfg")" = "$MARKER" ]; then echo ours
	else echo foreign
	fi
}

# ---- Secure Boot -------------------------------------------------------------

# on | off | unknown | no-efi
secure_boot() {
	if [ ! -d "$ROOT/sys/firmware/efi" ]; then echo no-efi; return; fi
	sb_f="$ROOT/sys/firmware/efi/efivars/$SB_VAR"
	if [ ! -r "$sb_f" ]; then echo unknown; return; fi
	# 4 bytes of attributes, then the value
	case "$(od -An -tu1 -j4 -N1 "$sb_f" 2>/dev/null | tr -d ' ')" in
		1) echo on ;; 0) echo off ;; *) echo unknown ;;
	esac
}
check_secure_boot() {
	case "$(secure_boot)" in
		on) die "Secure Boot is on; turn it off in the firmware setup (Limine and the Atari kernel are unsigned)" ;;
		unknown) warn "cannot read the Secure Boot state; if it is on, the Atari will not boot (turn it off in the firmware setup)" ;;
	esac
}

# ---- partitions ----------------------------------------------------------------

lsblk1() { lsblk -dno "$1" "$2" 2>/dev/null | sed 's/[[:space:]]*$//'; }
# Filesystem type: blkid as root (exact), lsblk otherwise (udev's cache).
fstype_of() {
	ft=''
	[ "$IS_ROOT" = 1 ] && ft=$(blkid -p -s TYPE -o value "$1" 2>/dev/null || true)
	[ -n "$ft" ] || ft=$(lsblk1 FSTYPE "$1")
	echo "$ft"
}
in_fstab() { # $1 device; prints the matching fstab line
	[ -f "$ROOT/etc/fstab" ] || return 1
	if_real=$(readlink -f "$1")
	if_uuid=$(lsblk1 UUID "$1") if_puuid=$(lsblk1 PARTUUID "$1") if_label=$(lsblk1 LABEL "$1")
	while read -r if_spec if_rest; do
		case "$if_spec" in ''|\#*) continue ;; esac
		if_spec=$(printf '%s' "$if_spec" | tr -d '"')
		if_hit=0
		case "$if_spec" in
			UUID=*) [ -n "$if_uuid" ] && [ "${if_spec#UUID=}" = "$if_uuid" ] && if_hit=1 ;;
			PARTUUID=*) [ -n "$if_puuid" ] && [ "${if_spec#PARTUUID=}" = "$if_puuid" ] && if_hit=1 ;;
			LABEL=*) [ -n "$if_label" ] && [ "$(printf '%b' "${if_spec#LABEL=}" | sed 's/\\040/ /g')" = "$if_label" ] && if_hit=1 ;;
			/dev/*) [ "$(readlink -f "$if_spec" 2>/dev/null)" = "$if_real" ] && if_hit=1 ;;
		esac
		if [ "$if_hit" = 1 ]; then echo "$if_spec $if_rest"; return 0; fi
	done < "$ROOT/etc/fstab"
	return 1
}
# Sets PV_OK (1/0), PV_WHY (refusal reasons, "; " separated), PV_NOTE,
# PV_FSTYPE, PV_SIZE for partition $1. Contents are not looked at here.
part_verdict() {
	PV_OK=1 PV_WHY='' PV_NOTE='' PV_FSTYPE='' PV_SIZE=0
	pv_add() { PV_OK=0; PV_WHY="${PV_WHY:+$PV_WHY; }$1"; }
	if [ ! -b "$1" ]; then pv_add "not a block device"; return; fi
	pv_type=$(lsblk1 TYPE "$1")
	[ "$pv_type" = part ] || pv_add "not a partition (lsblk TYPE ${pv_type:-?}); give a partition like /dev/sdb3, never a whole disk"
	PV_SIZE=$(lsblk -bdno SIZE "$1" 2>/dev/null | tr -d ' ') || PV_SIZE=0
	[ -n "$PV_SIZE" ] || PV_SIZE=0
	[ "$PV_SIZE" -ge "$MIN_PART_BYTES" ] || pv_add "too small ($(human "$PV_SIZE"); needs 1.5 GiB)"
	pv_mnt=$(findmnt -rno TARGET -S "$1" 2>/dev/null | tr '\n' ' ' | sed 's/ $//') || pv_mnt=''
	[ -z "$pv_mnt" ] || pv_add "mounted at $pv_mnt"
	if grep -qs "^$(readlink -f "$1") " /proc/swaps; then pv_add "in use as swap"; fi
	pv_fst=$(in_fstab "$1") && pv_add "in /etc/fstab ($pv_fst)"
	pv_name=$(basename "$(readlink -f "$1")")
	pv_holders=''
	for pv_h in "/sys/class/block/$pv_name/holders"/*; do
		[ -e "$pv_h" ] && pv_holders="${pv_holders:+$pv_holders }$(basename "$pv_h")"
	done
	[ -z "$pv_holders" ] || pv_add "in use by $pv_holders (device mapper or RAID)"
	pv_pt=$(lsblk1 PARTTYPE "$1" | tr 'A-F' 'a-f')
	case "$pv_pt" in "$ESP_GUID"|0xef) pv_add "the EFI system partition" ;; esac
	PV_FSTYPE=$(fstype_of "$1")
	case "$PV_FSTYPE" in
		crypto_LUKS|LVM2_member|linux_raid_member|swap|BitLocker|zfs_member|bcache) pv_add "holds $PV_FSTYPE" ;;
		vfat)
			if [ "$FORMAT" = 1 ]; then PV_NOTE="vfat; will be formatted"
			else PV_NOTE="vfat; must be empty or an earlier Atari install"; fi ;;
		'')
			if [ "$FORMAT" = 1 ]; then PV_NOTE="no filesystem; will be formatted"
			else pv_add "no filesystem (use --format)"; fi ;;
		*)
			if [ "$FORMAT" = 1 ]; then PV_NOTE="$PV_FSTYPE; will be ERASED and formatted"
			else pv_add "holds $PV_FSTYPE, not FAT32 (--format erases it)"; fi ;;
	esac
}
# Top-level entries of a mounted FAT that are not ours ("" when none).
foreign_files() {
	ff_out=''
	for ff_e in "$1"/* "$1"/.[!.]* "$1"/..?*; do
		[ -e "$ff_e" ] || continue
		ff_n=$(basename "$ff_e")
		case "$ff_n" in
			atari-os|atari|limine*|"System Volume Information") continue ;;
			EFI)
				# only EFI/BOOT, and in it only the removable-media loaders
				ff_bad=0
				for ff_x in "$ff_e"/*; do [ -e "$ff_x" ] || continue; [ "$(basename "$ff_x")" = BOOT ] || ff_bad=1; done
				for ff_x in "$ff_e"/BOOT/*; do
					[ -e "$ff_x" ] || continue
					case "$(basename "$ff_x" | tr '[:lower:]' '[:upper:]')" in BOOTX64.EFI|BOOTIA32.EFI) ;; *) ff_bad=1 ;; esac
				done
				[ "$ff_bad" = 0 ] && continue ;;
		esac
		ff_out="${ff_out:+$ff_out, }$ff_n"
	done
	echo "$ff_out"
}

# ---- the GRUB entry ---------------------------------------------------------------

# grub_entry_partition SERIAL CMDLINE
grub_entry_partition() {
	cat <<EOF
menuentry "$NAME" --id $MENU_ID {
	insmod part_msdos
	insmod part_gpt
	insmod fat
	set gfxpayload=keep
	search --no-floppy --fs-uuid --set=root $1
	linux /atari-os/vmlinuz $2 atari.media=$1
	initrd /atari-os/initramfs
}
EOF
}
# grub_entry_image GRUB_FS_MODULE UUID PATH CMDLINE
grub_entry_image() {
	cat <<EOF
menuentry "$NAME" --id $MENU_ID {
	insmod part_msdos
	insmod part_gpt
	insmod $1
	insmod loopback
	insmod fat
	set gfxpayload=keep
	search --no-floppy --fs-uuid --set=root $2
	loopback atari $3
	linux (atari,msdos1)/atari-os/vmlinuz $4 atari.image=$2:$3
	initrd (atari,msdos1)/atari-os/initramfs
}
EOF
}
# The whole custom.cfg: marker, where the files are (for --remove), entry.
custom_cfg_text() { # $1 "where" line, then the entry on stdin
	printf '%s\n# %s\n# Written by atari-os-x86 scripts/install-linux.sh; GRUB reads this file\n# through /etc/grub.d/41_custom. Delete it (or run --remove) to drop the entry.\n\n' "$MARKER" "$1"
	cat
}
# Kernel options from a limine.conf on stdin: its first cmdline:, without
# our own media options (they are added per install).
first_cmdline() {
	sed -n 's/^[[:space:]]*cmdline:[[:space:]]*//p' | head -n 1 \
		| tr -s '[:blank:]' '\n' | grep -v -e '^atari\.media=' -e '^atari\.image=' | tr '\n' ' ' | sed 's/ *$//'
}

GRUB_DIR='' GRUB_MANUAL=''
# Find grub.cfg and decide whether we may write custom.cfg. When we may
# not, GRUB_MANUAL says why: the files are still installed and the entry
# is printed at the end, to be added by hand (exit status 2).
grub_preflight() {
	GRUB_MANUAL=''
	if ! find_grub_cfg; then
		if [ -n "$GRUB_SEEN" ]; then
			GRUB_MANUAL="this GRUB does not read custom.cfg (no 41_custom in${GRUB_SEEN})"
		else
			GRUB_MANUAL="no grub.cfg found in /boot/grub, /boot/grub2 or /boot/efi/EFI/*"
		fi
		echo "  GRUB: $GRUB_MANUAL; the entry is printed at the end for you to add" >&2
		return 0
	fi
	GRUB_DIR=$(dirname "$GRUB_CFG")
	[ "$GRUB_GUESS" = 0 ] || warn "$GRUB_CFG is not readable without sudo; assuming it reads custom.cfg (/etc/grub.d/41_custom is enabled)"
	gp_state=$(custom_state "$GRUB_DIR")
	if [ "$gp_state" = foreign ]; then
		GRUB_MANUAL="$GRUB_DIR/custom.cfg exists and is not ours (it is left alone)"
		echo "  GRUB: $GRUB_MANUAL; the entry is printed at the end for you to add" >&2
		return 0
	fi
	echo "  GRUB: write $GRUB_DIR/custom.cfg ($gp_state) with the menu entry \"$NAME\"" >&2
}
# Write custom.cfg, or print the entry when grub_preflight said we may not.
# $1 the "where" line, $2 the entry.
write_custom_cfg() {
	if [ -n "$GRUB_MANUAL" ]; then
		echo >&2
		warn "$GRUB_MANUAL"
		echo "The files are installed, but the GRUB menu entry is not. Add it yourself, e.g. to" >&2
		echo "/etc/grub.d/40_custom (then run update-grub, or grub2-mkconfig -o <your grub.cfg>):" >&2
		printf '\n%s\n\n' "$2"
		return 0
	fi
	wc_tmp="$GRUB_DIR/custom.cfg.atari-new"
	printf '%s\n' "$2" | custom_cfg_text "$1" > "$wc_tmp"
	mv -f "$wc_tmp" "$GRUB_DIR/custom.cfg"
	sync
	say "Wrote $GRUB_DIR/custom.cfg"
}
# --dry-run: what would be written ($1 entry, $2 "where" line)
dry_entry() {
	if [ -n "$GRUB_MANUAL" ]; then
		echo "The entry to add by hand would be:" >&2
		printf '%s\n' "$1"
	else
		echo "$GRUB_DIR/custom.cfg would be:" >&2
		printf '%s\n' "$1" | custom_cfg_text "$2"
	fi
}
# Exit status: 0 with the menu entry in place, 2 when it must be added by hand.
finish() { if [ -n "$GRUB_MANUAL" ]; then exit 2; fi; exit 0; }
# How to get into the menu, from /etc/default/grub (read, never written).
menu_hints() {
	mh_style=$(grub_default GRUB_TIMEOUT_STYLE)
	mh_timeout=$(grub_default GRUB_TIMEOUT)
	mh_default=$(grub_default GRUB_DEFAULT)
	if [ "$mh_style" = hidden ] || [ "$mh_timeout" = 0 ]; then
		echo "The GRUB menu is hidden on this machine (GRUB_TIMEOUT_STYLE=${mh_style:-menu}, GRUB_TIMEOUT=${mh_timeout:-?})." >&2
		echo "To see it: hold Shift (BIOS) or press Esc (UEFI) right after power-on." >&2
	else
		echo "\"$NAME\" is in the GRUB menu at the next start (after the generated entries)." >&2
	fi
	if [ "$mh_default" = saved ]; then
		echo "GRUB_DEFAULT=saved: to start the Atari once, run  sudo $(grub_reboot_cmd) $MENU_ID && sudo reboot" >&2
	else
		echo "($(grub_reboot_cmd) $MENU_ID would need GRUB_DEFAULT=saved in /etc/default/grub; that is ${mh_default:-0} here and is not changed.)" >&2
	fi
}

# ---- --check -----------------------------------------------------------------------

do_check() {
	echo "atari-os-x86 install-linux.sh --check (nothing is changed)"
	echo
	if [ "$IS_ROOT" = 1 ]; then echo "Running as root: yes"; else echo "Running as root: no (installing needs sudo; some details below need it too)"; fi
	if [ -d "$ROOT/sys/firmware/efi" ]; then echo "Firmware: UEFI"; else echo "Firmware: BIOS (or no /sys/firmware/efi)"; fi
	echo "GRUB: $(grub_flavour)"
	if find_grub_cfg; then
		if [ "$GRUB_GUESS" = 1 ]; then
			echo "grub.cfg: $GRUB_CFG (not readable without sudo; /etc/grub.d/41_custom is enabled, so it most likely reads custom.cfg)"
		else
			echo "grub.cfg: $GRUB_CFG (reads custom.cfg: yes)"
		fi
		case "$(custom_state "$(dirname "$GRUB_CFG")")" in
			unreadable) echo "custom.cfg: exists, not readable without sudo" ;;
			none) echo "custom.cfg: none yet (this script would create it)" ;;
			ours) echo "custom.cfg: ours (an earlier install; it would be replaced)"
			      sed -n '2s/^# /  /p' "$(dirname "$GRUB_CFG")/custom.cfg" ;;
			foreign) echo "custom.cfg: exists and is NOT ours (the entry would have to be added by hand)" ;;
		esac
	elif [ -n "$GRUB_SEEN" ]; then
		echo "grub.cfg:$GRUB_SEEN (reads custom.cfg: NO; the entry would have to go into /etc/grub.d/40_custom by hand)"
	else
		echo "grub.cfg: not found (looked in /boot/grub, /boot/grub2, /boot/efi/EFI/*)"
	fi
	echo "Menu: GRUB_DEFAULT=$(grub_default GRUB_DEFAULT) GRUB_TIMEOUT=$(grub_default GRUB_TIMEOUT) GRUB_TIMEOUT_STYLE=$(grub_default GRUB_TIMEOUT_STYLE)"
	case "$(secure_boot)" in
		on) echo "Secure Boot: ON (must be turned off in the firmware setup first)" ;;
		off) echo "Secure Boot: off" ;;
		unknown) echo "Secure Boot: unknown (cannot read the efivar)" ;;
		no-efi) echo "Secure Boot: n/a (BIOS)" ;;
	esac
	ck_missing=''
	for ck_t in blkid lsblk findmnt unzip mkfs.vfat xz sfdisk; do have "$ck_t" || ck_missing="$ck_missing $ck_t"; done
	echo "Tools missing:${ck_missing:- none}"
	echo
	echo "Partitions (--partition; --format would allow the ones that only need formatting):"
	printf '  %-16s %9s  %-12s %-12s %s\n' DEVICE SIZE FSTYPE LABEL VERDICT
	lsblk -rpno NAME,TYPE 2>/dev/null | while read -r ck_dev ck_type; do
		[ "$ck_type" = part ] || continue
		part_verdict "$ck_dev"
		if [ "$PV_OK" = 1 ]; then ck_v="ok ($PV_NOTE)"; else ck_v="no: $PV_WHY"; fi
		printf '  %-16s %9s  %-12s %-12s %s\n' "$ck_dev" "$(human "$PV_SIZE")" "${PV_FSTYPE:--}" \
			"$(lsblk1 LABEL "$ck_dev" | cut -c1-12)" "$ck_v"
	done
	echo
	echo "Loopback (--image): any directory on an ext4, btrfs or xfs filesystem with room for the image, e.g."
	findmnt -rno TARGET,FSTYPE,SOURCE -t ext4,ext3,ext2,btrfs,xfs 2>/dev/null | while read -r ck_t ck_f ck_s; do
		ck_avail=$(df -Pk "$ck_t" 2>/dev/null | awk 'NR == 2 { print $4 * 1024 }')
		printf '  %-24s %-6s %-20s %s free\n' "$ck_t" "$ck_f" "$ck_s" "$(human "${ck_avail:-0}")"
	done
	exit 0
}

# ---- --remove ----------------------------------------------------------------------

do_remove() {
	rm_found=0
	for rm_d in "$ROOT/boot/grub" "$ROOT/boot/grub2" "$ROOT"/boot/efi/EFI/*; do
		[ -d "$rm_d" ] || continue
		case "$(custom_state "$rm_d")" in
			ours)
				rm_found=1
				rm_where=$(sed -n '2s/^# //p' "$rm_d/custom.cfg")
				if [ "$DRY" = 1 ]; then
					echo "Would delete $rm_d/custom.cfg (dry run)"
				else
					rm -f "$rm_d/custom.cfg"
					sync
					say "Deleted $rm_d/custom.cfg: the \"Atari\" entry is gone from the GRUB menu"
				fi
				[ -n "$rm_where" ] && echo "The files stay: $rm_where (delete them yourself if you want the space back)." >&2
				;;
			foreign) echo "$rm_d/custom.cfg is not ours (no marker on its first line); left alone." >&2 ;;
		esac
	done
	[ "$rm_found" = 1 ] || echo "No custom.cfg of ours found; nothing to remove." >&2
	exit 0
}

# ---- --partition -------------------------------------------------------------------

do_partition() {
	need_tools blkid lsblk findmnt unzip mount umount
	[ "$FORMAT" = 0 ] || need_tools mkfs.vfat wipefs
	[ -f "$SOURCE" ] || die "no such file: $SOURCE"
	case "$SOURCE" in *.zip) ;; *) die "partition mode takes the files zip (atari-os-x86-<v>-files.zip), not $SOURCE" ;; esac
	zl=$(unzip -Z1 "$SOURCE" 2>/dev/null) || die "$SOURCE is not a readable zip"
	for zf in limine.conf atari-os/vmlinuz atari-os/initramfs atari-os/modloop.sqfs; do
		printf '%s\n' "$zl" | grep -qx "$zf" || die "$SOURCE has no $zf: not an atari-os-x86 files zip"
	done
	CMDLINE=$(unzip -p "$SOURCE" limine.conf | first_cmdline)
	[ -n "$CMDLINE" ] || CMDLINE=$DEFAULT_CMDLINE
	ZIP_VERSION=$(unzip -p "$SOURCE" atari-os/VERSION 2>/dev/null | head -n 1) || ZIP_VERSION=''

	check_secure_boot
	DEV=$TARGET
	case "$DEV" in /dev/*) ;; *) DEV=/dev/$DEV ;; esac
	part_verdict "$DEV"
	if [ "$PV_OK" = 0 ]; then
		echo "Refused: $DEV: $PV_WHY" >&2
		echo "(sh $0 --check lists the partitions and what is wrong with each.)" >&2
		exit 1
	fi
	# Contents of an existing FAT: only ours, unless --format.
	if [ "$PV_FSTYPE" = vfat ] && [ "$FORMAT" = 0 ]; then
		if [ "$IS_ROOT" = 1 ]; then
			mnt=$(mktemp -d /tmp/atari-check.XXXXXX)
			mount -t vfat -o ro "$DEV" "$mnt" || { rmdir "$mnt"; die "cannot mount $DEV to look at it"; }
			foreign=$(foreign_files "$mnt")
			umount "$mnt"; rmdir "$mnt"
			[ -z "$foreign" ] || die "$DEV holds other files ($foreign); use another partition, or --format to erase it"
		else
			warn "not root: the contents of $DEV were not checked (the install checks them)"
		fi
	fi
	disk=$(lsblk1 PKNAME "$DEV")
	pttype=$(lsblk1 PTTYPE "$DEV")
	partno=$(cat "/sys/class/block/$(basename "$(readlink -f "$DEV")")/partition" 2>/dev/null || echo '')
	settype=0
	if [ "$pttype" = dos ]; then
		case "$(lsblk1 PARTTYPE "$DEV")" in 0xb|0xc|0xe|0x6|0x0b|0x0c|0x0e|0x06) ;; *) settype=1 ;; esac
	fi
	model=$(lsblk1 MODEL "/dev/$disk")

	echo "Plan:" >&2
	echo "  partition $DEV ($(human "$PV_SIZE"), $PV_FSTYPE${PV_FSTYPE:+, }on /dev/$disk ${model:+$model, }${pttype:-?} table)" >&2
	[ "$FORMAT" = 0 ] || echo "  - ERASE it: wipefs -a, mkfs.vfat -F 32 -n ATARI -s 8" >&2
	[ "$settype" = 0 ] || echo "  - set its MBR partition type to 0c (FAT32 LBA): sfdisk --part-type /dev/$disk $partno 0c" >&2
	echo "  - unzip $(basename "$SOURCE")${ZIP_VERSION:+ ($ZIP_VERSION)} onto it (files of an earlier install are replaced, your atari/ files stay)" >&2
	echo "  - add atari.media=<its serial> to its limine.conf" >&2
	if [ "$FORMAT" = 1 ]; then serial_shown='<serial after formatting>'; else serial_shown=$(blkid -s UUID -o value "$DEV" 2>/dev/null || lsblk1 UUID "$DEV"); fi
	entry=$(grub_entry_partition "${serial_shown:-XXXX-XXXX}" "$CMDLINE")
	grub_preflight
	if [ "$DRY" = 1 ]; then
		echo >&2
		dry_entry "$entry" "partition $DEV"
		echo "Dry run: nothing written." >&2
		exit 0
	fi
	echo >&2
	if [ "$FORMAT" = 1 ]; then
		echo "${RED}Everything on $DEV ($(human "$PV_SIZE")${PV_FSTYPE:+, $PV_FSTYPE}$(lsblk1 LABEL "$DEV" | sed 's/^./, label &/')) is erased.${OFF}" >&2
	fi
	printf 'Type the device name (%s) to confirm: ' "$DEV" >&2
	read -r answer || answer=''
	[ "$answer" = "$DEV" ] || { echo "That is not $DEV. Cancelled, nothing written." >&2; exit 1; }
	if [ -z "$GRUB_MANUAL" ]; then and_grub=' and add the GRUB entry'; else and_grub=''; fi
	confirm_yes "Write to $DEV$and_grub? Type yes to continue: "

	if [ "$FORMAT" = 1 ]; then
		say "Formatting $DEV"
		wipefs -a -q "$DEV"
		mkfs.vfat -F 32 -n ATARI -s 8 "$DEV" >/dev/null
	fi
	if [ "$settype" = 1 ] && [ -n "$partno" ]; then
		say "Setting the partition type of $DEV to 0c"
		sfdisk --no-reread --no-tell-kernel --part-type "/dev/$disk" "$partno" 0c >/dev/null 2>&1 \
			|| warn "could not set the partition type (harmless for GRUB; set it to 0c with fdisk if Windows or the firmware should see it)"
	fi
	blkid -c /dev/null "$DEV" >/dev/null 2>&1 || true
	SERIAL=$(blkid -c /dev/null -s UUID -o value "$DEV" 2>/dev/null || true)
	case "$SERIAL" in [0-9A-F][0-9A-F][0-9A-F][0-9A-F]-[0-9A-F][0-9A-F][0-9A-F][0-9A-F]) ;;
		*) die "cannot read the FAT32 serial of $DEV (blkid says '${SERIAL}')" ;; esac
	[ "$(blkid -c /dev/null -s TYPE -o value "$DEV")" = vfat ] || die "$DEV is not FAT after all"

	mnt=$(mktemp -d /tmp/atari-install.XXXXXX)
	say "Copying the files to $DEV"
	mount -t vfat -o shortname=mixed "$DEV" "$mnt" || { rmdir "$mnt"; die "cannot mount $DEV"; }
	if ! unzip -o -q "$SOURCE" -d "$mnt"; then
		umount "$mnt" || true; rmdir "$mnt"
		die "copying failed (disk full?); $DEV is mounted no more, the GRUB entry was not written"
	fi
	# atari.media=<serial> on every cmdline: (what the F8 disk installer
	# does), in case this FAT is ever started through its own Limine.
	if [ -f "$mnt/limine.conf" ]; then
		sed "/^[[:space:]]*cmdline:/{/atari\.media=/!s/\$/ atari.media=$SERIAL/;}" "$mnt/limine.conf" > "$mnt/limine.conf.new"
		mv -f "$mnt/limine.conf.new" "$mnt/limine.conf"
	fi
	sync
	umount "$mnt"
	rmdir "$mnt"
	write_custom_cfg "partition $DEV (FAT32 serial $SERIAL)" "$(grub_entry_partition "$SERIAL" "$CMDLINE")"
	if [ -z "$GRUB_MANUAL" ]; then
		echo >&2
		say "Done: \"$NAME\" boots from $DEV."
		menu_hints
		echo "To remove the entry again: sudo sh $0 --remove (the partition and its files stay)." >&2
	fi
	finish
}

# ---- --image -----------------------------------------------------------------------

do_image() {
	need_tools findmnt df
	[ -f "$SOURCE" ] || die "no such file: $SOURCE"
	case "$SOURCE" in
		*.img.xz|*.xz)
			need_tools xz
			IMG_BYTES=$(xz --robot --list "$SOURCE" 2>/dev/null | awk -F'\t' '$1 == "totals" {print $5}') || IMG_BYTES=''
			[ -n "$IMG_BYTES" ] || die "$SOURCE is not a valid .xz file"
			;;
		*.img) IMG_BYTES=$(file_size "$SOURCE") ;;
		*) die "loopback mode takes the image (.img or .img.xz), not $SOURCE" ;;
	esac
	[ "$IMG_BYTES" -gt 0 ] && [ $((IMG_BYTES % 512)) = 0 ] || die "$SOURCE is not a disk image"

	# The destination: a new file; a directory gets the image's name in it.
	DEST=$TARGET
	if [ -d "$DEST" ]; then
		base=$(basename "$SOURCE"); base=${base%.xz}
		DEST="${DEST%/}/$base"
	fi
	destdir=$(dirname "$DEST")
	[ -d "$destdir" ] || die "no such directory: $destdir"
	destdir=$(cd "$destdir" && pwd -P)
	DEST="$destdir/$(basename "$DEST")"
	case "$DEST" in
		*[!A-Za-z0-9._/+-]*) die "the image path may only use letters, digits and . _ + - / (GRUB and the kernel command line see it): $DEST" ;;
	esac
	case "$(basename "$DEST")" in *.img) ;; *) die "the image file name must end in .img: $DEST" ;; esac
	if [ -e "$DEST" ] && [ "$FORCE" = 0 ]; then
		die "$DEST exists; it is never overwritten (--force replaces it)"
	fi
	[ "$(readlink -f "$SOURCE")" != "$DEST" ] || die "source and destination are the same file"

	fs_type=$(findmnt -no FSTYPE -T "$destdir" | head -n 1)
	fs_target=$(findmnt -no TARGET -T "$destdir" | head -n 1)
	fs_source=$(findmnt -no SOURCE -T "$destdir" | head -n 1)
	fs_root=$(findmnt -no FSROOT -T "$destdir" | head -n 1)
	fs_uuid=$(findmnt -no UUID -T "$destdir" 2>/dev/null | head -n 1)
	fs_opts=$(findmnt -no OPTIONS -T "$destdir" | head -n 1)
	case "$fs_type" in
		ext2|ext3|ext4) grub_mod=ext2 ;;
		btrfs) grub_mod=btrfs ;;
		xfs) grub_mod=xfs ;;
		*) die "$destdir is on ${fs_type:-an unknown filesystem}; loopback mode needs ext4, btrfs or xfs (GRUB and the Atari both read it)" ;;
	esac
	# btrfs: the device part before "[/subvol]"; FSROOT is the subvolume
	fs_dev=${fs_source%%\[*}
	if [ -z "$fs_uuid" ] && [ -b "$fs_dev" ]; then
		fs_uuid=$(blkid -s UUID -o value "$fs_dev" 2>/dev/null || lsblk1 UUID "$fs_dev")
	fi
	[ -n "$fs_uuid" ] || die "cannot find the UUID of the filesystem on $fs_source (run with sudo)"
	case "$fs_uuid" in *[!0-9A-Za-z-]*) die "unexpected filesystem UUID '$fs_uuid'" ;; esac
	# GRUB and init must be able to open what holds it: no LUKS, no LVM.
	if [ -b "$fs_dev" ]; then
		stack=$(lsblk -sno TYPE,FSTYPE "$fs_dev" 2>/dev/null | tr '\n' ' ') || stack=''
		case " $stack " in
			*crypt*) die "$destdir is on an encrypted volume ($fs_dev); GRUB and the Atari cannot open it. Use an unencrypted ext4/btrfs/xfs partition, or --partition." ;;
		esac
		case " $stack " in
			*" lvm "*|*LVM2_member*) die "$destdir is on LVM ($fs_dev); the Atari's init cannot open it. Use a plain partition, or --partition." ;;
		esac
		case " $stack " in
			*raid*) die "$destdir is on software RAID ($fs_dev); not supported. Use a plain partition, or --partition." ;;
		esac
	fi
	# The path as GRUB and init see it: from the filesystem's root (a
	# btrfs subvolume or a bind mount has its own FSROOT).
	rel=${DEST#"$fs_target"}
	[ "$fs_target" = / ] && rel=$DEST
	case "$fs_root" in ''|/) ;; *) rel="${fs_root%/}$rel" ;; esac
	case "$rel" in /*) ;; *) rel="/$rel" ;; esac

	avail=$(df -Pk "$destdir" | awk 'NR == 2 { print $4 }')
	avail=$((avail * 1024))
	have_old=0
	[ -f "$DEST" ] && have_old=$(file_size "$DEST")
	[ $((avail + have_old)) -ge $((IMG_BYTES + SPARE_BYTES)) ] \
		|| die "not enough room on $fs_target: $(human "$avail") free, the image needs $(human "$IMG_BYTES") plus 64 MiB"

	check_secure_boot
	CMDLINE=''
	echo "Plan:" >&2
	echo "  - copy $(basename "$SOURCE") ($(human "$IMG_BYTES")) to $DEST" >&2
	echo "    on $fs_source ($fs_type, UUID $fs_uuid, mounted at $fs_target); GRUB path $rel" >&2
	if [ "$fs_type" = btrfs ]; then
		echo "  - btrfs: chattr +C on the new file first (no copy-on-write for a disk image)" >&2
		case "$fs_opts" in *compress*) warn "this btrfs is mounted with compression ($fs_opts); chattr +C keeps the image uncompressed, which GRUB needs" ;; esac
	fi
	entry_preview=$(grub_entry_image "$grub_mod" "$fs_uuid" "$rel" "<kernel options from the image>")
	grub_preflight
	if [ "$DRY" = 1 ]; then
		echo >&2
		dry_entry "$entry_preview" "image $DEST"
		echo "Dry run: nothing written." >&2
		exit 0
	fi
	echo >&2
	if [ -z "$GRUB_MANUAL" ]; then and_grub=' and add the GRUB entry'; else and_grub=''; fi
	confirm_yes "Copy the image$and_grub? Type yes to continue: "

	part="$DEST.part"
	rm -f "$part"
	: > "$part"
	if [ "$fs_type" = btrfs ]; then
		if have chattr; then chattr +C "$part" 2>/dev/null || warn "chattr +C failed; the image may be copy-on-write (slower, and with compression GRUB cannot read it)"
		else warn "no chattr (e2fsprogs): run  chattr +C  on an empty file yourself if this matters"; fi
	fi
	say "Copying the image (this takes a minute)"
	sparse=''
	cp --help 2>&1 | grep -q -- '--sparse' && sparse='--sparse=always'
	# shellcheck disable=SC2216 # GNU cp reads the pipe through /dev/stdin
	case "$SOURCE" in
		*.xz) if [ -n "$sparse" ]; then xz -dc "$SOURCE" | cp $sparse /dev/stdin "$part"; else xz -dc "$SOURCE" > "$part"; fi ;;
		*) if [ -n "$sparse" ]; then cp $sparse "$SOURCE" "$part"; else cat "$SOURCE" > "$part"; fi ;;
	esac || { rm -f "$part"; die "copying failed"; }
	sync
	[ "$(file_size "$part")" = "$IMG_BYTES" ] || { rm -f "$part"; die "the copy has the wrong size (disk full?)"; }
	mv -f "$part" "$DEST"
	sync

	# Kernel options: the image's own limine.conf (first entry).
	CMDLINE=$(image_cmdline "$DEST")
	if [ -z "$CMDLINE" ]; then
		warn "could not read limine.conf inside the image; using the default kernel options"
		CMDLINE=$DEFAULT_CMDLINE
	fi
	write_custom_cfg "image $DEST" "$(grub_entry_image "$grub_mod" "$fs_uuid" "$rel" "$CMDLINE")"
	echo "The Atari's own files (library, settings, FujiNet SD) live inside $DEST." >&2
	if [ -z "$GRUB_MANUAL" ]; then
		echo >&2
		say "Done: \"$NAME\" boots the image $DEST."
		menu_hints
		echo "To remove the entry again: sudo sh $0 --remove (then delete $DEST yourself)." >&2
	fi
	finish
}
# limine.conf's first cmdline from inside an image file (FAT at 1 MiB):
# mtools when there, else a read-only loop mount (root).
image_cmdline() {
	if have mtype; then
		MTOOLS_SKIP_CHECK=1 mtype -i "$1@@1048576" ::limine.conf 2>/dev/null | tr -d '\r' | first_cmdline && return 0
	fi
	if [ "$IS_ROOT" = 1 ]; then
		ic_mnt=$(mktemp -d /tmp/atari-image.XXXXXX)
		if mount -t vfat -o ro,loop,offset=1048576 "$1" "$ic_mnt" 2>/dev/null; then
			first_cmdline < "$ic_mnt/limine.conf" || true
			umount "$ic_mnt"
		fi
		rmdir "$ic_mnt"
	fi
}

# The test hook (mk-grub-test.sh): source with INSTALL_LINUX_LIB=1 and a
# mode argument (e.g. --check) to get the entry functions without running.
[ "${INSTALL_LINUX_LIB:-0}" = 1 ] && return 0 2>/dev/null

case "$MODE" in
	check) do_check ;;
	remove) do_remove ;;
	partition) do_partition ;;
	image) do_image ;;
esac
