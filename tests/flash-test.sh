#!/usr/bin/env bash
# flash-test.sh - tests for scripts/flash.sh that never touch a disk
#
#   tests/flash-test.sh
#
# Writes only with --to-file into a scratch directory (FLASH_TEST_DIR, default
# a fresh mktemp -d). The disk list is faked with FLASH_TEST_LSBLK (canned
# lsblk -P output) and FLASH_TEST_PROC_MOUNTS/SWAPS; with any FLASH_TEST_*
# hook set flash.sh refuses to write a device, so the "pick a disk" paths
# are exercised up to that refusal.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FLASH="$HERE/../scripts/flash.sh"
T="${FLASH_TEST_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/flash-test.XXXXXX")}"
T="${T:?}"
mkdir -p "$T"
PASS=0 FAILS=0

ok()  { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAILS=$((FAILS + 1)); echo "FAIL $*"; sed 's/^/     | /' "$T/out" | tail -n 15; }

# run <name> <0|1: expected success or failure> <ERE that must be in the output, or ''> -- cmd...
run() {
	local name=$1 want=$2 pat=$3 rc=0
	shift 4
	"$@" > "$T/out" 2>&1 < "${STDIN:-/dev/null}" || rc=$?
	if [ "$want" = 0 ] && [ "$rc" != 0 ]; then bad "$name (exit $rc, wanted 0)"; return 0; fi
	if [ "$want" != 0 ] && [ "$rc" = 0 ]; then bad "$name (exit 0, wanted failure)"; return 0; fi
	if [ -n "$pat" ] && ! grep -qE -- "$pat" "$T/out"; then bad "$name (no /$pat/ in output)"; return 0; fi
	ok "$name"
}
same() { if cmp -s "$1" "$2"; then ok "$3"; else bad "$3"; fi; }

# ---- test images: 8 MiB (read back in 4 MiB blocks) and 1 MiB + 512 bytes (512-byte blocks)
head -c $((8 * 1024 * 1024)) /dev/urandom > "$T/a.img"
xz -T0 -1 -c "$T/a.img" > "$T/a.img.xz"
head -c $((1024 * 1024 + 512)) /dev/urandom > "$T/odd.img"
head -c 1000 /dev/urandom > "$T/notdisk.img"
: > "$T/out"

# ---- writing to a file
run "img to file" 0 "Verified" -- "$FLASH" --to-file "$T/copy.img" --yes "$T/a.img"
same "$T/a.img" "$T/copy.img" "img copy identical"
{ printf 'longer than the image\n'; head -c $((16 * 1024 * 1024)) /dev/zero; } > "$T/copy.img"
run "xz to file (over a longer file)" 0 "Verified" -- "$FLASH" --to-file "$T/copy.img" --yes "$T/a.img.xz"
same "$T/a.img" "$T/copy.img" "xz copy identical (old file truncated)"
run "odd size, 512-byte read-back" 0 "Verified" -- "$FLASH" --to-file "$T/copy2.img" --yes "$T/odd.img"
same "$T/odd.img" "$T/copy2.img" "odd copy identical"
printf 'yes\n' > "$T/in"
STDIN="$T/in" run "to file asks yes" 0 "Verified" -- "$FLASH" --to-file "$T/copy3.img" "$T/a.img"
printf 'no\n' > "$T/in"
STDIN="$T/in" run "to file, answer no" 1 "Cancelled" -- "$FLASH" --to-file "$T/copy4.img" "$T/a.img"
if [ ! -e "$T/copy4.img" ]; then ok "nothing written after no"; else bad "nothing written after no"; fi
run "dry run to file writes nothing" 0 "Dry run" -- "$FLASH" --to-file "$T/copy5.img" --dry-run "$T/a.img"
if [ ! -e "$T/copy5.img" ]; then ok "no file after dry run"; else bad "no file after dry run"; fi

# ---- verify must catch a corrupted write
FLASH_TEST_CORRUPT=1 run "corrupted img copy is caught" 1 "VERIFY FAILED" -- "$FLASH" --to-file "$T/copy.img" --yes "$T/a.img"
FLASH_TEST_CORRUPT=1 run "corrupted xz copy is caught" 1 "VERIFY FAILED" -- "$FLASH" --to-file "$T/copy.img" --yes "$T/a.img.xz"

# ---- refusals
run "to-file /dev/null refused" 1 "not a regular file" -- "$FLASH" --to-file /dev/null --yes "$T/a.img"
mkdir -p "$T/dir"
run "to-file directory refused" 1 "not a regular file" -- "$FLASH" --to-file "$T/dir" --yes "$T/a.img"
ln -sf /dev/null "$T/link-to-dev"
run "to-file symlink to a device refused" 1 "not a regular file" -- "$FLASH" --to-file "$T/link-to-dev" --yes "$T/a.img"
run "to-file onto the image refused" 1 "image itself" -- "$FLASH" --to-file "$T/a.img" --yes "$T/a.img"
run "--yes without --device" 1 "needs --device" -- "$FLASH" --yes "$T/a.img"
run "--device with --to-file" 1 "exclude each other" -- "$FLASH" --device sdb --to-file "$T/x.img" "$T/a.img"
run "not a disk image" 1 "multiple of 512" -- "$FLASH" --to-file "$T/x.img" --yes "$T/notdisk.img"
cp "$T/a.img" "$T/a.iso"
run "wrong extension" 1 "\.img or \.img\.xz" -- "$FLASH" --to-file "$T/x.img" --yes "$T/a.iso"
head -c 4096 /dev/urandom > "$T/broken.img.xz"
run "broken xz" 1 "not a valid \.xz" -- "$FLASH" --to-file "$T/x.img" --yes "$T/broken.img.xz"
run "missing image" 1 "no such file" -- "$FLASH" --to-file "$T/x.img" --yes "$T/nope.img"
run "unknown option" 1 "unknown option" -- "$FLASH" --force "$T/a.img"
run "--help" 0 "Usage: flash.sh" -- "$FLASH" --help
run "plain sh is refused" 1 "needs bash" -- sh "$FLASH" --help
if [ ! -e "$T/x.img" ]; then ok "no stray output file"; else bad "no stray output file"; fi

# ---- the disk list (faked; flash.sh never writes a disk with a hook set)
cat > "$T/lsblk" <<'L'
NAME="nvme0n1" PKNAME="" TYPE="disk" SIZE="1024209543168" RM="0" TRAN="nvme" MODEL="Samsung SSD 980" SERIAL="S1" MOUNTPOINT="" FSTYPE=""
NAME="nvme0n1p1" PKNAME="nvme0n1" TYPE="part" SIZE="536870912" RM="0" TRAN="nvme" MODEL="" SERIAL="" MOUNTPOINT="/boot/efi" FSTYPE="vfat"
NAME="nvme0n1p2" PKNAME="nvme0n1" TYPE="part" SIZE="1023000000000" RM="0" TRAN="nvme" MODEL="" SERIAL="" MOUNTPOINT="/" FSTYPE="ext4"
NAME="sda" PKNAME="" TYPE="disk" SIZE="1000204886016" RM="0" TRAN="usb" MODEL="WD\x20Elements" SERIAL="WX1" MOUNTPOINT="" FSTYPE=""
NAME="sda1" PKNAME="sda" TYPE="part" SIZE="1000204000000" RM="0" TRAN="usb" MODEL="" SERIAL="" MOUNTPOINT="/media/me/BACKUP" FSTYPE="ntfs"
NAME="sdb" PKNAME="" TYPE="disk" SIZE="15376318464" RM="1" TRAN="usb" MODEL="SanDisk\x20Cruzer\x20Blade" SERIAL="4C530001" MOUNTPOINT="" FSTYPE=""
NAME="sdb1" PKNAME="sdb" TYPE="part" SIZE="15375000000" RM="1" TRAN="usb" MODEL="" SERIAL="" MOUNTPOINT="/media/me/STICK" FSTYPE="vfat"
NAME="sdc" PKNAME="" TYPE="disk" SIZE="8004304896" RM="1" TRAN="usb" MODEL="Generic" SERIAL="G2" MOUNTPOINT="" FSTYPE=""
NAME="sdc1" PKNAME="sdc" TYPE="part" SIZE="8000000000" RM="1" TRAN="usb" MODEL="" SERIAL="" MOUNTPOINT="[SWAP]" FSTYPE="swap"
NAME="sdd" PKNAME="" TYPE="disk" SIZE="64023257088" RM="1" TRAN="usb" MODEL="Encrypted" SERIAL="E3" MOUNTPOINT="" FSTYPE=""
NAME="sdd1" PKNAME="sdd" TYPE="part" SIZE="64000000000" RM="1" TRAN="usb" MODEL="" SERIAL="" MOUNTPOINT="" FSTYPE="crypto_LUKS"
NAME="dm-0" PKNAME="sdd1" TYPE="crypt" SIZE="63900000000" RM="0" TRAN="" MODEL="" SERIAL="" MOUNTPOINT="/home" FSTYPE="ext4"
NAME="sde" PKNAME="" TYPE="disk" SIZE="0" RM="1" TRAN="usb" MODEL="Card\x20Reader" SERIAL="CR" MOUNTPOINT="" FSTYPE=""
NAME="sdf" PKNAME="" TYPE="disk" SIZE="32010928128" RM="1" TRAN="usb" MODEL="Var\x20Stick" SERIAL="V5" MOUNTPOINT="" FSTYPE=""
NAME="sdf1" PKNAME="sdf" TYPE="part" SIZE="32000000000" RM="1" TRAN="usb" MODEL="" SERIAL="" MOUNTPOINT="" FSTYPE="ext4"
NAME="sdg" PKNAME="" TYPE="disk" SIZE="4000000000" RM="1" TRAN="usb" MODEL="Tiny" SERIAL="T6" MOUNTPOINT="" FSTYPE=""
NAME="mmcblk0" PKNAME="" TYPE="disk" SIZE="31914983424" RM="0" TRAN="mmc" MODEL="" SERIAL="0x1234" MOUNTPOINT="" FSTYPE=""
NAME="mmcblk0p1" PKNAME="mmcblk0" TYPE="part" SIZE="31900000000" RM="0" TRAN="mmc" MODEL="" SERIAL="" MOUNTPOINT="" FSTYPE="vfat"
NAME="loop0" PKNAME="" TYPE="loop" SIZE="70049792" RM="0" TRAN="" MODEL="" SERIAL="" MOUNTPOINT="/snap/core24/1" FSTYPE="squashfs"
L
# sdf1 on /var is only in the kernel's mount table, sdg only in /proc/swaps.
printf '/dev/sdf1 /var ext4 rw 0 0\n/dev/sdb1 /media/me/STICK vfat rw 0 0\n' > "$T/mounts"
printf 'Filename\tType\tSize\tUsed\tPriority\n/dev/sdg\tpartition\t3906248\t0\t-2\n' > "$T/swaps"
export FLASH_TEST_LSBLK="$T/lsblk" FLASH_TEST_PROC_MOUNTS="$T/mounts" FLASH_TEST_PROC_SWAPS="$T/swaps"

run "list: offers sdb" 0 "^  1 +sdb +14\.3 GiB +SanDisk Cruzer Blade +4C530001 +/media/me/STICK" -- "$FLASH" --dry-run "$T/a.img"
if [ "$(grep -cE '^  [0-9]+ ' "$T/out")" = 2 ]; then ok "list: exactly two disks offered"; else bad "list: exactly two disks offered"; fi
if grep -qE "^  2 +mmcblk0 +29\.7 GiB" "$T/out"; then ok "list: offers mmcblk0"; else bad "list: offers mmcblk0"; fi
for why in "nvme0n1 \(holds /boot/efi\)" "sda \(larger than 512 GiB\)" "sdc \(holds swap\)" "sdd \(holds /home\)" \
	"sde \(no medium\)" "sdf \(holds /var\)" "sdg \(holds swap\)"; do
	if grep -qE "$why" "$T/out"; then ok "list: not offered: $why"; else bad "list: not offered: $why"; fi
done
run "device sdb dry run" 0 "Target: /dev/sdb \(14\.3 GiB, SanDisk Cruzer Blade\)" -- "$FLASH" --device sdb --dry-run "$T/a.img.xz"
run "device /dev/sdb accepted" 0 "Target: /dev/sdb" -- "$FLASH" --device /dev/sdb --dry-run "$T/a.img"
run "device nvme0n1 refused" 1 "nvme0n1 cannot be written: holds" -- "$FLASH" --device nvme0n1 --dry-run "$T/a.img"
run "device sda refused" 1 "sda cannot be written: larger than 512 GiB" -- "$FLASH" --device sda --yes "$T/a.img"
run "device sdd1 (a partition) refused" 1 "sdd1 cannot be written" -- "$FLASH" --device sdd1 --yes "$T/a.img"
run "device dm-0 refused" 1 "dm-0 cannot be written" -- "$FLASH" --device dm-0 --yes "$T/a.img"
run "unknown device refused" 1 "sdz cannot be written" -- "$FLASH" --device sdz --yes "$T/a.img"
run "device sdb --yes: the hook blocks the write" 1 "refusing to write a disk" -- "$FLASH" --device sdb --yes "$T/a.img"
printf 'sdb\nyes\n' > "$T/in"
STDIN="$T/in" run "interactive: name then yes reaches the write" 1 "refusing to write a disk" -- "$FLASH" "$T/a.img"
if grep -q "Everything on /dev/sdb (14.3 GiB, SanDisk Cruzer Blade, serial 4C530001) will be erased" "$T/out"; then
	ok "interactive: confirmation repeats size and model"
else
	bad "interactive: confirmation repeats size and model"
fi
printf '1\n' > "$T/in"
STDIN="$T/in" run "interactive: a number is refused" 1 "not the number" -- "$FLASH" "$T/a.img"
printf 'sda\n' > "$T/in"
STDIN="$T/in" run "interactive: an excluded disk is refused" 1 "sda cannot be written" -- "$FLASH" "$T/a.img"
printf 'sdb\nYES\n' > "$T/in"
STDIN="$T/in" run "interactive: only 'yes' confirms" 1 "Cancelled" -- "$FLASH" "$T/a.img"
printf '\n' > "$T/in"
STDIN="$T/in" run "interactive: Enter cancels" 1 "Cancelled" -- "$FLASH" "$T/a.img"
truncate -s 5G "$T/big.img"
FLASH_TEST_PROC_SWAPS=/dev/null \
	run "image larger than the disk" 1 "larger than /dev/sdg" -- "$FLASH" --device sdg --dry-run "$T/big.img"
FLASH_TEST_CORRUPT=1 run "corrupt hook never writes a disk" 1 "refusing to write a disk" -- "$FLASH" --device sdb --yes "$T/a.img"
unset FLASH_TEST_LSBLK FLASH_TEST_PROC_MOUNTS FLASH_TEST_PROC_SWAPS

for f in a.img a.img.xz odd.img notdisk.img copy.img copy2.img copy3.img a.iso broken.img.xz big.img \
	link-to-dev lsblk mounts swaps in out; do
	rm -f "${T:?}/${f:?}"
done
rmdir "${T:?}/dir" 2>/dev/null || true
[ -n "${FLASH_TEST_DIR:-}" ] || rmdir "${T:?}" 2>/dev/null || true

echo "$PASS passed, $FAILS failed"
[ "$FAILS" = 0 ]
