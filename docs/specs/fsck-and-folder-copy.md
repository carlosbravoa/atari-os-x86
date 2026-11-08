# Spec: FAT check at boot; copy whole folders in the Library

Status: from Carlos's session 2026-11-08. The ZenBook's stick ended up with
828 MB of lost clusters (47 real files; a TNFS download of an ATX that
"took a looong while" and some restarts that were not clean). Nothing
repairs a FAT on the device today, and copies failed with ENOSPC. Also:
"i would like to copy whole folders from my usb … large libraries".
Design: Claude (Fable); implementer: an Opus agent; verification: Claude.
Rules as in `docs/specs/menus.md` (patch regeneration, `-Werror`, docs in
the same change). Nothing in `scripts/flash.sh`, `flash-it.sh` untouched.

## A. fsck.fat before mounting FAT volumes (init)

- `dosfstools` is already in the stage (the installer's mkfs.fat); make
  sure `fsck.fat` (= dosfsck, 70 KB, needs ld-musl which is present) is in
  the initramfs (`scripts/mkimage.sh`, where mkfs.fat is copied).
- init runs `fsck.fat -a -w <partition>` **before** mounting read-write:
  the stick / image FAT in `try_mount_media()` and the installed-disk path
  (`init.c` ~970), and every vfat USB drive in `usb_mount()`. exfat/ntfs:
  no checker here, mount as today.
- Behaviour: run it always (a 1 GiB FAT takes under a second; it also
  clears the dirty bit and fixes the free-cluster count). Log one line
  with the result: `fsck: /dev/sda1: clean` / `fsck: /dev/sda1: repaired
  (reclaimed N clusters, M bytes)` / `fsck: /dev/sda1: failed (rc)`, then
  mount anyway. Exit code 0 = clean, 1 = repaired, other = error. Cap the
  run with a 60 s timeout (big FAT32 USB drives); on timeout kill it and
  mount anyway, log it.
- Splash: while the stick is checked show the progress line "Checking the
  stick..." (same mechanism as the other boot lines); nothing for USB
  drives (they hot-plug later; the border hint is enough).
- `status` gets `fsck <clean|repaired|failed|skipped>` for the stick, and
  the Hardware page shows `Stick: FAT clean` / `FAT repaired at boot`.
- KNOWN-ISSUES: lost clusters after an unclean stop are now reclaimed at
  the next boot; a stick pulled while writing can still lose the file
  being written.

## B. Copy a whole folder in the Library

- Browsing a folder on a **USB source** (any depth, including the drive
  root), the listing gets a header line under Drives:
  `[ Copy this folder to Library ]`. On the **Stick** source inside
  `library/` (or any folder of it) when a USB drive is present:
  `[ Copy this folder to USB... ]` (picks the drive as "Copy to..." does).
- RETURN on it: count first (files, total bytes, recursive; `.zip` counted
  as files), check free space on the destination with `statvfs`; refuse
  with "Needs 312 MB, Library has 201 MB free" when short. Then confirm
  `Copy "DEMOS" (230 files, 312 MB) to Library?` Yes/No.
- Destination keeps the folder: `library/<folder name>/...` (USB root:
  `library/<drive label>/...`). Files that already exist with the same
  size are skipped. Progress on the message line: `Copying 12/230:
  name.atr` (reuse `copy_file()`'s per-file progress). Esc between files
  cancels; what was copied stays. End: `Copied 228 files, 2 skipped,
  310 MB` (USB destination: "Safe to unplug" once synced, as today).
  Log one line per folder copy (not per file) plus failures.
- After the copy the listing is redrawn; the Drives/Recent lines
  unchanged. `library.cfg` untouched.

## C. Docs

README Library section (folder copy), "Boot" paragraph (FAT check),
KNOWN-ISSUES, CHANGELOG.

## D. Verification (Claude, QEMU)

1. Boot the debug image: log shows `fsck: ... clean`. Stop QEMU hard
   (`qemu-test.sh stop` without shutdown) and boot again: `repaired` or
   `clean` with the dirty bit gone (`fsck.fat -n` on the image from the
   host shows no dirty bit).
2. USB test image attached: the Library's USB tab shows the copy line;
   copying `DEMOS` to Library lands in `atari/library/DEMOS/` with the
   same file sizes; a second copy skips everything; Esc mid-copy stops.
3. Free-space refusal: a destination with less free space than the
   folder needs (fill the test stick or use a tiny USB image).
