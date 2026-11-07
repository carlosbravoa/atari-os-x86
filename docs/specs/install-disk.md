# Spec: install onto the laptop's own disk (whole disk, from the appliance)

Status: agreed 2026-11-07 ("Old laptops could have this as a single
partition"; Windows users mostly convert the whole machine). Design: Claude
(Fable); implementer: an Opus agent; QEMU and laptop verification: Claude.
Rules as in `docs/specs/library.md` §3/§7 (C89 `-Werror`, musl, no new
libraries beyond Alpine packages named here, don't commit, report).

Two sibling specs come later and are **not** part of this one: a Linux
host script (GRUB drop-in `/boot/grub/custom.cfg` or BLS entry, plus a
loopback-image mode) and a PowerShell installer for UEFI dual boot with
Windows.

## 1. What the user sees

Boot the stick on the laptop that is going to become an Atari. F8 →
`Install onto this laptop's disk…`. The item exists only when the system
runs from a USB stick or an image file (`status` says `media stick` or
`media image-file`) and at least one internal disk was found; otherwise it
is absent. Flow, all with the UI driver like the rest of sysmenu.c:

1. Disk choice when there is more than one: `fSelect` popup
   `Samsung SSD 860 (500 GB)`, `SanDisk SD card (32 GB)`.
2. Info page (`fInfoScreen`): "This erases EVERYTHING on <model>, <size>.
   The laptop will then boot the Atari from its own disk, BIOS or UEFI,
   and the USB stick is no longer needed. Nothing else can be kept."
3. `fEditString("Type ERASE to continue")`; anything but `ERASE` cancels.
4. Progress messages (`fMessage(…, FALSE)`): `Partitioning…`,
   `Formatting…`, `Copying system… 42%`, `Installing boot loader…`,
   `Registering with the firmware…`, then an info page `Installed. Remove
   the USB stick and press RETURN to reboot.` RETURN → `reboot`.
5. Any failure: info page with the step and the error text; nothing else
   happens; the stick keeps running.

## 2. Mechanism (in init, driven over the control socket)

New socket commands:

- `disks` → one line per candidate `disk <name> <bytes> <model>` and `OK`.
  Candidates: `/sys/block/*` that are not removable (`removable` = 0), not
  loop/ram/zram, not among `boot_disks[]` (the stick and the Ventoy host),
  not USB-attached (same ancestry test as the USB-drive code), size ≥ 4
  GiB. NVMe, SATA, eMMC (`mmcblk*` non-removable) all count.
- `install <name>` → streams progress lines `step <text>`, `progress <pct>`,
  then `OK` or `ERR <text>`; the emulator reads them as they come
  (`ctl_request` needs a streaming variant) and updates the message. init
  does the work in a forked child so its main loop keeps running (the
  emulator is still showing the picture); only one install at a time.

Steps, with the tools added to the image (`scripts/mkimage.sh` stage
packages): `sfdisk` (util-linux-misc), `dosfstools` (`mkfs.vfat`),
`limine` (the `limine` CLI; the EFI and BIOS stage files are already on
the stick), plus busybox `blkid`:

1. **Partition**: `sfdisk --wipe always /dev/X` with a script that makes an
   MBR with one partition, type `0c`, bootable, from 1 MiB to the end,
   mirroring `mkimage.sh`'s layout so Limine's BIOS install works the same
   way. Re-read the table (`BLKRRPART` ioctl or `blockdev --rereadpt`) and
   wait for the partition node.
2. **Format**: `mkfs.vfat -F 32 -n ATARI -s 8 /dev/X1` (8-sector clusters
   like the image). Read the volume serial with `blkid -s UUID -o value`.
3. **Copy**: mount at `/mnt/install` (vfat, `shortname=mixed`); copy the
   running media `/media` (the FAT, **not** `/media/usb` and not
   `/.host`): `busybox cp -a` per top-level entry, progress from a `du -s`
   total versus bytes copied so far (sample `df` of the target every
   second from the parent). Then edit the copy's `limine.conf`: append
   `atari.media=<serial>` to both entries' cmdline (§3).
4. **Boot loader**: `limine bios-install /dev/X` (writes the MBR code and
   stage into the gap before 1 MiB; the FAT already has `limine-bios.sys`,
   `EFI/BOOT/BOOTX64.EFI`, `limine.conf`).
5. **Firmware entry (UEFI only, i.e. `/sys/firmware/efi/efivars` mounted)**:
   create a `Boot####` load option "Atari" whose device path is
   `HD(1, MBR, <disk signature>, <start LBA>, <size LBA>)/\EFI\BOOT\BOOTX64.EFI`
   (take the signature from the MBR bytes 440..443 that sfdisk wrote, or
   write a random one yourself and pass it to sfdisk with `label-id`),
   and put it **first in `BootOrder`**. This is the one place the project
   edits the boot order, and only because the user has just typed ERASE on
   a machine that will have no other operating system; say so in the
   README. Implementation goes next to the existing `efi_bootnext()` (same
   efivarfs read/write and immutable-flag handling). On BIOS machines
   nothing to do: the MBR is active.
6. `sync`, unmount, `OK`.

Also: `status` gains `media stick|image-file|disk|none` (init knows
`media_kind` already; add `disk` for a non-removable, non-USB media
device) and `installable <n>` (candidate count), so sysmenu can show or
hide the item without a second round trip.

## 3. Which copy boots when both are present

After installing, the stick and the internal disk both hold
`/atari-os/modloop.sqfs`. init's media scan must prefer the copy it was
booted from: `atari.media=<vfat serial>` on the kernel command line, set
by the installer in the installed `limine.conf`, makes `scan_block_devices`
try the partition with that serial first (compare with
`blkid -s UUID -o value` or by reading the FAT boot sector's volume ID at
offset 0x43 directly — the latter avoids a fork per partition). Without the
parameter the order is as today. mkimage.sh does **not** set it for the
stick image (the serial is the same for every copy of the image, so it
would say nothing). The other copy is then treated as "another atari-os
stick, not used" by the USB code, which is already the case.

## 4. Verification (QEMU)

- Add `scripts/mk-install-test.sh` → `build/install-test.img`, 8 GiB
  sparse, blank. Attach it as a **non-removable** disk:
  `QEMU_EXTRA="-drive if=none,id=hd0,format=raw,file=build/install-test.img -device ide-hd,drive=hd0"`
  (or `-device virtio-blk-pci`; both must appear as installable; the test
  USB drive from `mk-usb-test.sh` must not).
- Boot the stick with it (BIOS and UEFI; OVMF needs `-snapshot` off for the
  target disk only: add `QEMU_WRITABLE_EXTRA=1` or equivalent so the extra
  drive is not snapshotted; say how). Screenshots: the F8 item, the disk
  popup (when two targets are attached), the ERASE prompt, a progress
  screen, the final page. Log lines `install: …` for every step.
- Then boot QEMU **from the installed disk alone** (`QEMU_IMG=build/install-test.img`,
  BIOS and UEFI): the emulator comes up, `status` says `media disk`, the
  version matches, and F8 has no install item. With the stick attached as
  well and `atari.media` in place, `/run/init.log`/dmesg shows the
  internal copy chosen and the stick listed as another atari-os stick.
- Negative: cancel at the ERASE prompt (nothing written: `blkid` on the
  target still empty); `install` of a non-candidate name → `ERR`.
- UEFI entry: in the installed-disk UEFI boot, `efibootmgr`-equivalent
  check by reading `BootOrder` and the `Boot####` variable from efivarfs
  over serial (busybox `hexdump`), and confirm OVMF boots it without the
  removable-media fallback (remove `EFI/BOOT/BOOTX64.EFI` temporarily?
  no: just confirm the entry exists and is first).

README: an "Install onto the laptop's disk" section under Install; a
KNOWN-ISSUES note that Secure Boot must be off (Limine is unsigned) and
that the installer makes one partition only.
