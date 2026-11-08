#!/usr/bin/env bash
# flash.sh - write an atari-os-x86 image to a USB stick, without any chance
# of picking the wrong disk.
#
#   ./flash.sh atari-os-x86-v0.1.0.img.xz            pick a stick, confirm, write, verify
#   ./flash.sh --device sdb atari-os-x86-v0.1.0.img  pre-select (still asks "yes")
#   ./flash.sh --dry-run atari-os-x86-v0.1.0.img.xz  show the sticks and the plan, write nothing
#   ./flash.sh --to-file copy.img image.img.xz       write to a regular file instead
#
# Linux and macOS, bash 3.2 or later. Only removable/USB/SD disks that hold
# no system mount (/, /boot, /home, swap ...), that do not hold the image
# itself and that are at most 512 GiB are offered; nothing else can be
# written, and there is no option to override that. sudo is used for the
# unmount, the write, the read-back and the eject, nothing else.
#
# Licence: GPL-2.0-or-later, like the rest of atari-os-x86.

if [ -z "${BASH_VERSION:-}" ]; then
	echo "flash.sh needs bash: run it as  bash flash.sh ..." >&2
	exit 1
fi
set -euo pipefail

MAX_BYTES=$((512 * 1024 * 1024 * 1024))   # larger disks are never offered
CRITICAL_MOUNTS="/ /boot /boot/efi /home /usr /var"

# ---- output helpers ---------------------------------------------------------

if [ -t 2 ]; then
	RED=$'\033[1;31m' BLUE=$'\033[1;34m' BOLD=$'\033[1m' OFF=$'\033[0m'
else
	RED='' BLUE='' BOLD='' OFF=''
fi
say() { printf '%s==>%s %s\n' "$BLUE" "$OFF" "$*" >&2; }
die() { printf '%serror:%s %s\n' "$RED" "$OFF" "$*" >&2; exit 1; }
fail_red() { printf '%s%s%s\n' "$RED" "$*" "$OFF" >&2; }

usage() {
	cat <<'EOF'
Usage: flash.sh [options] IMAGE

Write an atari-os-x86 image (.img or .img.xz) to a USB stick or SD card
and verify it. Linux and macOS.

Options:
  --device NAME     pre-select the stick (sdb, mmcblk0, disk4); it must be
                    one of the offered disks, and "yes" is still asked
  --yes             do not ask for "yes" (needs --device or --to-file)
  --dry-run         list the disks and print the plan; write nothing
  --to-file PATH    write to a regular file instead of a disk (a copy for
                    Ventoy, or tests); never a device node
  -h, --help        this text

Only removable, USB or SD disks up to 512 GiB that hold no system mount
point, no swap and not the image itself are offered. You type the device
name (not a number), then confirm with "yes" after a line that repeats its
size and model. The stick is read back and compared (SHA-256) after
writing.

Windows: use balenaEtcher or Rufus (DD mode). Ventoy: copy the .img
(after unxz) onto the Ventoy stick instead.
EOF
}

# ---- arguments ------------------------------------------------------------

DEVICE='' YES=0 DRY=0 TO_FILE='' IMAGE=''
while [ $# -gt 0 ]; do
	case "$1" in
		--device) [ $# -ge 2 ] || die "--device needs a name"; DEVICE=$2; shift ;;
		--device=*) DEVICE=${1#--device=} ;;
		--yes) YES=1 ;;
		--dry-run) DRY=1 ;;
		--to-file) [ $# -ge 2 ] || die "--to-file needs a path"; TO_FILE=$2; shift ;;
		--to-file=*) TO_FILE=${1#--to-file=} ;;
		-h|--help) usage; exit 0 ;;
		--) shift; break ;;
		-*) die "unknown option $1 (see --help)" ;;
		*) [ -z "$IMAGE" ] || die "only one image, please (see --help)"; IMAGE=$1 ;;
	esac
	shift
done
if [ $# -gt 0 ]; then
	[ -z "$IMAGE" ] && [ $# -eq 1 ] || die "only one image, please (see --help)"
	IMAGE=$1
fi
[ -n "$IMAGE" ] || { usage >&2; exit 2; }
DEVICE=${DEVICE#/dev/}
[ -n "$TO_FILE" ] && [ -n "$DEVICE" ] && die "--device and --to-file exclude each other"
[ "$YES" = 1 ] && [ -z "$DEVICE" ] && [ -z "$TO_FILE" ] && die "--yes needs --device NAME (the disk is never picked for you)"

# Test hooks (tests/flash-test.sh). They can only make things fail or
# fake the disk list; with any of them set, no disk is ever written.
TEST_LSBLK=${FLASH_TEST_LSBLK:-}
TEST_CORRUPT=${FLASH_TEST_CORRUPT:-0}
PROC_MOUNTS=${FLASH_TEST_PROC_MOUNTS:-/proc/mounts}
PROC_SWAPS=${FLASH_TEST_PROC_SWAPS:-/proc/swaps}
TESTING=0
if [ -n "$TEST_LSBLK" ] || [ "$TEST_CORRUPT" != 0 ] || [ -n "${FLASH_TEST_PROC_MOUNTS:-}" ] || [ -n "${FLASH_TEST_PROC_SWAPS:-}" ]; then
	TESTING=1
fi

# ---- platform and tools -----------------------------------------------------

OS=$(uname -s)
case "$OS" in
	Linux|Darwin) ;;
	*) die "unsupported system '$OS': flash.sh runs on Linux and macOS (on Windows use balenaEtcher or Rufus in DD mode)" ;;
esac

need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1 ($2)"; }
need dd "coreutils"
case "$IMAGE" in *.xz) need xz "install xz-utils; macOS: brew install xz, or unxz the image elsewhere" ;; esac
if command -v sha256sum >/dev/null 2>&1; then
	sha256() { sha256sum | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
	sha256() { shasum -a 256 | awk '{print $1}'; }
else
	die "missing tool: sha256sum or shasum"
fi
if [ -z "$TO_FILE" ]; then
	if [ "$OS" = Linux ]; then
		[ -n "$TEST_LSBLK" ] || need lsblk "util-linux"
	else
		need diskutil "macOS"
		need /usr/libexec/PlistBuddy "macOS"
	fi
fi

if [ "$(id -u)" = 0 ]; then SUDO=''; else SUDO=sudo; fi

file_size() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }

human() { # bytes -> "14.9 GiB"
	awk -v b="$1" 'BEGIN {
		split("B KiB MiB GiB TiB", u, " "); i = 1
		while (b >= 1024 && i < 5) { b /= 1024; i++ }
		fmt = (i == 1) ? "%d %s" : "%.1f %s"
		printf fmt, b, u[i] }'
}

# ---- the image --------------------------------------------------------------

[ -f "$IMAGE" ] || die "no such file: $IMAGE"
case "$IMAGE" in
	*.img.xz|*.xz)
		IMG_BYTES=$(xz --robot --list "$IMAGE" 2>/dev/null | awk -F'\t' '$1 == "totals" {print $5}') || IMG_BYTES=''
		[ -n "$IMG_BYTES" ] || die "$IMAGE is not a valid .xz file"
		read_image() { xz -dc "$IMAGE"; }
		;;
	*.img)
		IMG_BYTES=$(file_size "$IMAGE")
		read_image() { cat "$IMAGE"; }
		;;
	*) die "the image must be a .img or .img.xz file" ;;
esac
[ "$IMG_BYTES" -gt 0 ] || die "$IMAGE is empty"
[ $((IMG_BYTES % 512)) = 0 ] || die "$IMAGE is not a disk image (size is not a multiple of 512 bytes)"

# Which disk holds the image (it is never offered).
IMAGE_SRC=$(df -P "$IMAGE" 2>/dev/null | awk 'NR == 2 {print $1}') || IMAGE_SRC=''

# ---- candidate disks: Linux -------------------------------------------------

# Parallel arrays (bash 3.2: no associative arrays).
L_NAME=() L_PK=() L_TYPE=() L_SIZE=() L_RM=() L_TRAN=() L_MODEL=() L_SERIAL=() L_MNT=() L_FST=()
C_NAME=() C_SIZE=() C_MODEL=() C_SERIAL=() C_MNT=()   # offered
X_NAME=() X_WHY=()                                     # disks not offered, and why
BAD_DISKS=' '                                          # " sda nvme0n1 " etc.

unhex() { printf '%b' "$1" | sed -e 's/[[:space:]]*$//'; }

lsblk_rows() {
	if [ -n "$TEST_LSBLK" ]; then
		cat "$TEST_LSBLK"
	else
		lsblk -P -b -o NAME,PKNAME,TYPE,SIZE,RM,TRAN,MODEL,SERIAL,MOUNTPOINT,FSTYPE
	fi
}

linux_load() {
	local line rest key val n=0
	local NAME PKNAME TYPE SIZE RM TRAN MODEL SERIAL MOUNTPOINT FSTYPE
	L_NAME=() L_PK=() L_TYPE=() L_SIZE=() L_RM=() L_TRAN=() L_MODEL=() L_SERIAL=() L_MNT=() L_FST=()
	while IFS= read -r line; do
		NAME='' PKNAME='' TYPE='' SIZE=0 RM=0 TRAN='' MODEL='' SERIAL='' MOUNTPOINT='' FSTYPE=''
		rest=$line
		# KEY="value" pairs; lsblk hex-escapes quotes and other unsafe bytes.
		while [[ $rest =~ ^[[:space:]]*([A-Z_:]+)=\"([^\"]*)\"(.*)$ ]]; do
			key=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]} rest=${BASH_REMATCH[3]}
			case "$key" in
				NAME) NAME=$val ;; PKNAME) PKNAME=$val ;; TYPE) TYPE=$val ;;
				SIZE) SIZE=${val:-0} ;; RM) RM=$val ;; TRAN) TRAN=$val ;;
				MODEL) MODEL=$(unhex "$val") ;; SERIAL) SERIAL=$(unhex "$val") ;;
				MOUNTPOINT) MOUNTPOINT=$(unhex "$val") ;; FSTYPE) FSTYPE=$val ;;
			esac
		done
		[ -n "$NAME" ] || continue
		L_NAME[n]=$NAME L_PK[n]=$PKNAME L_TYPE[n]=$TYPE L_SIZE[n]=$SIZE L_RM[n]=$RM
		L_TRAN[n]=$TRAN L_MODEL[n]=$MODEL L_SERIAL[n]=$SERIAL L_MNT[n]=$MOUNTPOINT L_FST[n]=$FSTYPE
		n=$((n + 1))
	done < <(lsblk_rows)
	[ "$n" -gt 0 ] || die "lsblk listed no block devices"
}

# Disks a device sits on (walking PKNAME up; a device can have several parents).
disks_of() {
	local name=$1 depth=${2:-0} i
	[ "$depth" -lt 16 ] || return 0
	for i in "${!L_NAME[@]}"; do
		[ "${L_NAME[i]}" = "$name" ] || continue
		if [ "${L_TYPE[i]}" = disk ]; then
			echo "$name"
		elif [ -n "${L_PK[i]}" ]; then
			disks_of "${L_PK[i]}" $((depth + 1))
		fi
	done
}

mark_bad() { # device-name reason
	local d
	for d in $(disks_of "$1" | sort -u); do
		case "$BAD_DISKS" in *" $d "*) ;; *) BAD_DISKS="$BAD_DISKS$d "; X_NAME+=("$d"); X_WHY+=("$2") ;; esac
	done
}

kname_of() { # /dev/... path (or symlink) -> kernel name
	local p=$1
	case "$p" in /dev/*) ;; *) return 0 ;; esac
	if [ -z "$TEST_LSBLK" ]; then p=$(readlink -f "$p" 2>/dev/null || echo "$p"); fi
	basename "$p"
}

is_critical() {
	local m
	for m in $CRITICAL_MOUNTS; do [ "$1" = "$m" ] && return 0; done
	return 1
}

linux_candidates() {
	local i j src target rest d mnts why
	linux_load
	# 1. system mounts and swap, from lsblk and from the kernel's own tables
	for i in "${!L_NAME[@]}"; do
		if is_critical "${L_MNT[i]}"; then mark_bad "${L_NAME[i]}" "holds ${L_MNT[i]}"; fi
		if [ "${L_MNT[i]}" = "[SWAP]" ] || [ "${L_FST[i]}" = swap ]; then mark_bad "${L_NAME[i]}" "holds swap"; fi
	done
	if [ -r "$PROC_MOUNTS" ]; then
		while read -r src target rest; do
			is_critical "$target" || continue
			d=$(kname_of "$src")
			[ -n "$d" ] && mark_bad "$d" "holds $target"
		done < "$PROC_MOUNTS"
	fi
	if [ -r "$PROC_SWAPS" ]; then
		while read -r src rest; do
			d=$(kname_of "$src")
			[ -n "$d" ] && mark_bad "$d" "holds swap"
		done < <(tail -n +2 "$PROC_SWAPS")
	fi
	# 2. the disk the image is read from
	if [ -n "$IMAGE_SRC" ]; then
		d=$(kname_of "$IMAGE_SRC")
		[ -n "$d" ] && mark_bad "$d" "holds the image file"
	fi
	# 3. what is left: whole disks that are removable, USB or SD, with media, small enough
	for i in "${!L_NAME[@]}"; do
		[ "${L_TYPE[i]}" = disk ] || continue
		d=${L_NAME[i]}
		case "$BAD_DISKS" in *" $d "*) continue ;; esac
		why=''
		if [ "${L_RM[i]}" != 1 ] && [ "${L_TRAN[i]}" != usb ] && [ "${L_TRAN[i]}" != mmc ]; then
			why="internal (${L_TRAN[i]:-no transport}, not removable)"
		elif [ "${L_SIZE[i]}" -le 0 ]; then
			why="no medium"
		elif [ "${L_SIZE[i]}" -gt "$MAX_BYTES" ]; then
			why="larger than 512 GiB"
		fi
		if [ -n "$why" ]; then
			X_NAME+=("$d") X_WHY+=("$why")
			continue
		fi
		mnts=''
		for j in "${!L_NAME[@]}"; do
			[ -n "${L_MNT[j]}" ] || continue
			if disks_of "${L_NAME[j]}" | grep -qx "$d"; then mnts="${mnts:+$mnts, }${L_MNT[j]}"; fi
		done
		C_NAME+=("$d") C_SIZE+=("${L_SIZE[i]}") C_MODEL+=("${L_MODEL[i]:--}")
		C_SERIAL+=("${L_SERIAL[i]:--}") C_MNT+=("${mnts:--}")
	done
}

# ---- candidate disks: macOS -------------------------------------------------

plist_get() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }

mac_whole() { echo "$1" | sed -E 's#^/dev/##; s#^r?(disk[0-9]+).*#\1#'; }

mac_candidates() {
	local tmp id size name internal mnts root_disks store why
	tmp=$(mktemp "${TMPDIR:-/tmp}/flash.XXXXXX")
	# The disks behind / (an external boot disk is still the system disk).
	root_disks=' '
	if diskutil info -plist / > "$tmp" 2>/dev/null; then
		root_disks="$root_disks$(mac_whole "$(plist_get "$tmp" ParentWholeDisk)") "
		store=$(plist_get "$tmp" "APFSPhysicalStores:0:APFSPhysicalStore")
		[ -n "$store" ] && root_disks="$root_disks$(mac_whole "$store") "
	fi
	[ -n "$IMAGE_SRC" ] && case "$IMAGE_SRC" in /dev/disk*) BAD_DISKS="$BAD_DISKS$(mac_whole "$IMAGE_SRC") " ;; esac
	for id in $(diskutil list external physical 2>/dev/null | awk '/^\/dev\/disk[0-9]+/ {print $1}'); do
		id=${id#/dev/}
		if ! diskutil info -plist "$id" > "$tmp" 2>/dev/null; then continue; fi
		size=$(plist_get "$tmp" TotalSize); [ -n "$size" ] || size=$(plist_get "$tmp" Size)
		size=${size:-0}
		name=$(plist_get "$tmp" MediaName)
		internal=$(plist_get "$tmp" Internal)
		why=''
		case "$root_disks" in *" $id "*) why="holds /" ;; esac
		case "$BAD_DISKS" in *" $id "*) why="holds the image file" ;; esac
		if [ -z "$why" ]; then
			if [ "$internal" = true ]; then why="internal"
			elif [ "$size" -le 0 ]; then why="no medium"
			elif [ "$size" -gt "$MAX_BYTES" ]; then why="larger than 512 GiB"
			fi
		fi
		if [ -n "$why" ]; then X_NAME+=("$id") X_WHY+=("$why"); continue; fi
		mnts=$(mount | sed -n -E "s#^/dev/${id}(s[0-9]+)? on (.*) \\(.*#\\2#p" | paste -sd, - | sed 's/,/, /g')
		C_NAME+=("$id") C_SIZE+=("$size") C_MODEL+=("${name:--}") C_SERIAL+=("-") C_MNT+=("${mnts:--}")
	done
	rm -f "$tmp"
}

scan() {
	C_NAME=() C_SIZE=() C_MODEL=() C_SERIAL=() C_MNT=() X_NAME=() X_WHY=() BAD_DISKS=' '
	if [ "$OS" = Linux ]; then linux_candidates; else mac_candidates; fi
}

show_table() {
	local i
	if [ "${#C_NAME[@]}" -eq 0 ]; then
		echo "No USB stick or SD card found. Plug one in and run this again." >&2
	else
		printf '\n  %-3s %-10s %-10s %-28s %-20s %s\n' '#' DEVICE SIZE MODEL SERIAL MOUNTED >&2
		for i in "${!C_NAME[@]}"; do
			printf '  %-3s %-10s %-10s %-28.28s %-20.20s %s\n' "$((i + 1))" "${C_NAME[i]}" \
				"$(human "${C_SIZE[i]}")" "${C_MODEL[i]}" "${C_SERIAL[i]}" "${C_MNT[i]}" >&2
		done
	fi
	if [ "${#X_NAME[@]}" -gt 0 ]; then
		printf '\n  Not offered:' >&2
		for i in "${!X_NAME[@]}"; do printf ' %s (%s)%s' "${X_NAME[i]}" "${X_WHY[i]}" \
			"$([ "$i" -lt $((${#X_NAME[@]} - 1)) ] && echo ',')" >&2; done
		printf '\n' >&2
	fi
	echo >&2
}

find_candidate() { # name -> index, or nothing
	local i
	[ "${#C_NAME[@]}" -gt 0 ] || return 0
	for i in "${!C_NAME[@]}"; do [ "${C_NAME[i]}" = "$1" ] && { echo "$i"; return 0; }; done
	return 0
}

why_not() {
	local i
	[ "${#X_NAME[@]}" -gt 0 ] || { echo "not a disk on this machine"; return 0; }
	for i in "${!X_NAME[@]}"; do [ "${X_NAME[i]}" = "$1" ] && { echo "${X_WHY[i]}"; return 0; }; done
	echo "not a disk this script knows"
}

# ---- the target ---------------------------------------------------------------

if [ -n "$TO_FILE" ]; then
	if [ -e "$TO_FILE" ] && [ ! -f "$TO_FILE" ]; then
		die "--to-file $TO_FILE exists and is not a regular file (device nodes are never written this way)"
	fi
	case "$TO_FILE" in /dev/*) [ -f "$TO_FILE" ] || die "--to-file must not point into /dev" ;; esac
	if [ -e "$TO_FILE" ] && [ "$TO_FILE" -ef "$IMAGE" ]; then die "--to-file is the image itself"; fi
	TARGET=$TO_FILE TARGET_DESC="file $TO_FILE"
else
	scan
	if [ -z "$DEVICE" ]; then
		show_table
		[ "${#C_NAME[@]}" -gt 0 ] || exit 1
		if [ "$DRY" = 1 ]; then
			echo "Dry run: re-run with --device NAME to see the plan for one of these." >&2
			exit 0
		fi
		printf 'Type the device name to write (for example %s), or press Enter to cancel: ' "${C_NAME[0]}" >&2
		read -r DEVICE || DEVICE=''
		DEVICE=${DEVICE#/dev/}
		[ -n "$DEVICE" ] || { echo "Cancelled." >&2; exit 1; }
		case "$DEVICE" in
			*[!0-9]*) ;;
			*) die "type the device name (like ${C_NAME[0]}), not the number" ;;
		esac
	fi
	IDX=$(find_candidate "$DEVICE")
	if [ -z "$IDX" ]; then
		[ "${#C_NAME[@]}" -gt 0 ] && show_table
		die "$DEVICE cannot be written: $(why_not "$DEVICE"). Only the disks listed above can."
	fi
	SIZE=${C_SIZE[IDX]} MODEL=${C_MODEL[IDX]} SERIAL=${C_SERIAL[IDX]}
	if [ "$OS" = Darwin ]; then
		TARGET=/dev/r$DEVICE   # raw device: much faster, no buffer cache
	else
		TARGET=/dev/$DEVICE
	fi
	TARGET_DESC="/dev/$DEVICE ($(human "$SIZE"), $MODEL)"
	[ "$IMG_BYTES" -le "$SIZE" ] || die "the image ($(human "$IMG_BYTES")) is larger than /dev/$DEVICE ($(human "$SIZE"))"
fi

# ---- the plan -----------------------------------------------------------------

DD_BS=4M
[ "$OS" = Darwin ] && DD_BS=4m
DD_PROGRESS=''
if dd if=/dev/zero of=/dev/null count=1 status=progress 2>/dev/null; then DD_PROGRESS='status=progress'; fi
FSYNC_TMP=$(mktemp "${TMPDIR:-/tmp}/flash.XXXXXX")
DD_FSYNC=''
if dd if=/dev/zero of="$FSYNC_TMP" count=1 conv=fsync 2>/dev/null; then DD_FSYNC='conv=fsync'; fi
rm -f "$FSYNC_TMP"

say "Image:  $IMAGE ($(human "$IMG_BYTES"))"
say "Target: $TARGET_DESC"
if [ "$DRY" = 1 ]; then
	if [ -n "$TO_FILE" ]; then PFX=''; else PFX=${SUDO:+sudo }; fi
	echo "Dry run, nothing written. The plan:" >&2
	if [ -z "$TO_FILE" ]; then echo "  - unmount every partition of /dev/$DEVICE" >&2; fi
	echo "  - compute the image's SHA-256" >&2
	echo "  - write it: ${PFX}dd of=$TARGET bs=$DD_BS $DD_FSYNC $DD_PROGRESS" >&2
	echo "  - read back $IMG_BYTES bytes and compare the SHA-256" >&2
	if [ -z "$TO_FILE" ]; then echo "  - sync and eject" >&2; fi
	exit 0
fi

if [ -z "$TO_FILE" ]; then
	if [ "$YES" != 1 ]; then
		echo >&2
		printf '%sEverything on /dev/%s (%s, %s, serial %s) will be erased.%s\n' "$BOLD" "$DEVICE" \
			"$(human "$SIZE")" "$MODEL" "$SERIAL" "$OFF" >&2
		[ -n "$SUDO" ] && echo "sudo will ask for your password to unmount, write, read back and eject it." >&2
		printf 'Type yes to continue: ' >&2
		read -r ANSWER || ANSWER=''
		[ "$ANSWER" = yes ] || { echo "Cancelled, nothing written." >&2; exit 1; }
	fi
	[ "$TESTING" = 0 ] || die "test hooks are set (FLASH_TEST_*): refusing to write a disk"
elif [ "$YES" != 1 ]; then
	printf 'Write the image to %s? Type yes to continue: ' "$TO_FILE" >&2
	read -r ANSWER || ANSWER=''
	[ "$ANSWER" = yes ] || { echo "Cancelled, nothing written." >&2; exit 1; }
fi

# ---- check, unmount, write ----------------------------------------------------

say "Checking the image (SHA-256)"
WANT=$(read_image | sha256) || die "could not read $IMAGE (damaged download?)"

if [ -z "$TO_FILE" ]; then
	# The same disk still there? (unplugged and another one took the name)
	scan
	IDX2=$(find_candidate "$DEVICE")
	if [ -z "$IDX2" ] || [ "${C_SIZE[IDX2]}" != "$SIZE" ] || [ "${C_MODEL[IDX2]}" != "$MODEL" ] || [ "${C_SERIAL[IDX2]}" != "$SERIAL" ]; then
		die "/dev/$DEVICE changed since you picked it; nothing written"
	fi
	say "Unmounting /dev/$DEVICE"
	if [ "$OS" = Darwin ]; then
		$SUDO diskutil unmountDisk "/dev/$DEVICE" >/dev/null || die "could not unmount /dev/$DEVICE"
	else
		# every partition and anything stacked on one (LUKS, LVM), as often
		# as needed for multiple mount points
		for try in 1 2 3 4 5 6; do
			MOUNTED=$(lsblk -lnpo NAME,MOUNTPOINT "/dev/$DEVICE" | awk 'NF > 1 {print $1}' | sort -u) \
				|| die "lsblk failed on /dev/$DEVICE"
			[ -n "$MOUNTED" ] || break
			[ "$try" -lt 6 ] || die "could not unmount: $(echo "$MOUNTED" | tr '\n' ' ')"
			for part in $MOUNTED; do $SUDO umount "$part" || true; done
		done
	fi
	[ "$OS" = Linux ] && [ ! -b "$TARGET" ] && die "$TARGET is not a block device"
fi

say "Writing"
if [ -n "$TO_FILE" ]; then
	# shellcheck disable=SC2086
	read_image | dd of="$TARGET" bs=$DD_BS $DD_FSYNC $DD_PROGRESS || die "writing $TARGET failed"
elif [ "$OS" = Darwin ]; then
	# ibs/obs instead of bs: dd then re-blocks short pipe reads into whole
	# 4 MiB writes, which the raw device needs.
	if [ -z "$DD_PROGRESS" ] && command -v pv >/dev/null 2>&1; then
		# shellcheck disable=SC2086
		read_image | pv -s "$IMG_BYTES" | $SUDO dd of="$TARGET" ibs=$DD_BS obs=$DD_BS $DD_FSYNC || die "writing $TARGET failed"
	else
		# shellcheck disable=SC2086
		read_image | $SUDO dd of="$TARGET" ibs=$DD_BS obs=$DD_BS $DD_FSYNC $DD_PROGRESS || die "writing $TARGET failed"
	fi
else
	if [ -z "$DD_PROGRESS" ] && command -v pv >/dev/null 2>&1; then
		# shellcheck disable=SC2086
		read_image | pv -s "$IMG_BYTES" | $SUDO dd of="$TARGET" bs=$DD_BS $DD_FSYNC || die "writing $TARGET failed"
	else
		# shellcheck disable=SC2086
		read_image | $SUDO dd of="$TARGET" bs=$DD_BS $DD_FSYNC $DD_PROGRESS || die "writing $TARGET failed"
	fi
fi
sync

# ---- verify -------------------------------------------------------------------

if [ -n "$TO_FILE" ] && [ "$TEST_CORRUPT" = 1 ]; then
	# test hook: flip the first byte of the written copy (files only)
	b=$(od -An -tu1 -N1 "$TARGET" | tr -d ' ')
	printf '%b' "\\0$(printf '%03o' $((255 - b)))" | dd of="$TARGET" bs=1 count=1 conv=notrunc 2>/dev/null
	echo "test: FLASH_TEST_CORRUPT=1, first byte of $TARGET changed" >&2
fi

say "Verifying (reading back $(human "$IMG_BYTES"))"
# Read exactly the image length, in whole blocks (no partial reads to trim).
if [ $((IMG_BYTES % 4194304)) = 0 ]; then VBS=4194304
elif [ $((IMG_BYTES % 1048576)) = 0 ]; then VBS=1048576
else VBS=512
fi
VCOUNT=$((IMG_BYTES / VBS))
if [ -n "$TO_FILE" ]; then
	GOT=$(dd if="$TARGET" bs=$VBS count=$VCOUNT 2>/dev/null | sha256) || GOT=''
else
	if [ "$OS" = Linux ]; then
		# skip the page cache: what is on the stick, not what was written to RAM
		$SUDO blockdev --flushbufs "$TARGET" 2>/dev/null || true
		GOT=$($SUDO dd if="$TARGET" bs=$VBS count=$VCOUNT iflag=direct 2>/dev/null | sha256) \
			|| GOT=$($SUDO dd if="$TARGET" bs=$VBS count=$VCOUNT 2>/dev/null | sha256) || GOT=''
	else
		GOT=$($SUDO dd if="$TARGET" bs=$VBS count=$VCOUNT 2>/dev/null | sha256) || GOT=''
	fi
fi
if [ "$GOT" != "$WANT" ]; then
	fail_red "VERIFY FAILED: what was read back from $TARGET does not match the image."
	fail_red "  image:     $WANT"
	fail_red "  read back: ${GOT:-(read failed)}"
	fail_red "Try another USB port or another stick; do not boot from this one."
	exit 1
fi
say "Verified: SHA-256 $WANT"

if [ -n "$TO_FILE" ]; then
	echo "Done: $TO_FILE is a copy of the image." >&2
	exit 0
fi

sync
if [ "$OS" = Darwin ]; then
	$SUDO diskutil eject "/dev/$DEVICE" >/dev/null 2>&1 && say "Ejected /dev/$DEVICE" || echo "Eject it from Finder before unplugging." >&2
elif command -v eject >/dev/null 2>&1; then
	$SUDO eject "/dev/$DEVICE" 2>/dev/null && say "Ejected /dev/$DEVICE" || echo "Unplug the stick once its light stops blinking." >&2
else
	echo "Unplug the stick once its light stops blinking." >&2
fi

cat >&2 <<'EOF'

Done. Next:
  1. Plug the stick into the laptop and open its boot menu while it starts
     (usually F12; F9 on HP, Esc or F8 on ASUS, F11 on MSI, the Option key
     on Intel Macs). Secure Boot must be off on UEFI machines.
  2. Pick the USB stick. The Atari comes up at READY.
  3. F1 = Help (every key), F8 = System menu (Wi-Fi, boot into
     BASIC/ASH/FujiNet, shut down), F9 = Library. Your files go in the
     stick's atari/ folder.

Ventoy users: no need for this script, copy the .img (after unxz) to the
Ventoy stick instead.
EOF
