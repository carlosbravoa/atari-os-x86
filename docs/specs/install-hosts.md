# Spec: install next to an existing Linux or Windows (dual boot)

Status: agreed 2026-11-07 ("i would like to install it into one partition
of my disk and configure grub to pick it up"; "it is no longer supposed to
manually edit that file"; "most of our users will use windows"). Design:
Claude (Fable); implementer: an Opus agent; verification: Claude in QEMU
(the Windows script cannot be run here: see §6). Rules as in
`docs/specs/library.md` §3/§7 and `docs/specs/install-disk.md` (C89
`-Werror`, musl, POSIX sh for scripts, don't commit, report). The
whole-disk installer (F8, `install-disk.md`) is done and is not touched.

## 0. Principles (from Carlos, not negotiable)

- **Nothing the user did not ask for is changed.** No `update-grub`, no
  edit of `grub.cfg` or `/etc/default/grub`, no change of the default boot
  entry, no change of the UEFI `BootOrder` default. The only firmware
  change the Windows script makes is to *append* its entry at the end of
  the firmware list (§4.4), so it shows in the F12 menu; what boots by
  default stays as it was. A one-shot boot (`BootNext`-like) is fine.
- **Safe by construction**: the scripts refuse anything that is not clearly
  a free target (§3.2, §4.2); they never pick a disk or partition on their
  own; every write to a partition is preceded by the user typing the
  device name and `yes`; `--dry-run`/`-WhatIf` shows the plan.
- **Reversible**: `--remove`/`-Remove` undoes exactly what was added
  (menu entry, firmware entry); the files on the partition are the user's
  to delete.
- Secure Boot must be **off** on both hosts (Limine and the Alpine kernel
  are unsigned; a shim-locked GRUB refuses an unsigned kernel too). The
  scripts detect it and stop with the firmware-setup hint instead of
  installing something that cannot boot.

## 1. What ships

`scripts/mkimage.sh` also produces **`build/atari-os-x86-<version>-files.zip`**:
the stick's FAT content as files (`limine.conf`, `limine-bios.sys`,
`EFI/BOOT/BOOTX64.EFI`, `atari-os/`, `atari/`), i.e. the staging dir it
already assembles (`$STICK`), zipped with `zip -r -X` (add `zip` to
`BUILD_PKGS` in `scripts/alpine-env.sh`). Without the developer key when
`DEV_SSH_KEY=0`, like the image. `release.yml` attaches it, plus
`scripts/install-linux.sh` and `scripts/install-windows.ps1`, and adds
their sha256 to `SHA256SUMS`; `.github/release-template.md` gets a
"Dual boot" paragraph (§5).

## 2. Two layouts

**Partition mode** (both hosts): a FAT32 partition of ≥ 1.5 GiB holds the
files from the zip; the host's boot loader (GRUB) or the firmware (Windows)
starts the kernel from it. init finds it through `atari.media=<FAT serial>`
on the command line exactly as for the whole-disk install (nothing new in
init for this mode). The partition is a plain FAT32 drive the user can
also fill from the host OS.

**Loopback mode** (Linux only): the release `.img` is copied as a file onto
an existing Linux partition (ext4, btrfs or xfs), GRUB loop-mounts it
(`loopback`) and boots kernel and initramfs from inside; init then finds
the image file through a new parameter and uses the FAT inside it as
`/media`, read-write. No partitioning at all. Not offered for Windows
(NTFS + Fast Startup leaves the volume dirty; `ntfs3` would mount it
read-only or refuse).

## 3. `scripts/install-linux.sh`

POSIX sh, `set -eu`, shellcheck-clean, runs with `sudo`. Usage:

```
install-linux.sh --partition /dev/sdXN  atari-os-x86-<v>-files.zip   # partition mode
install-linux.sh --image /path/on/a/linux/fs  atari-os-x86-<v>.img[.xz]   # loopback mode
install-linux.sh --remove        # drop the GRUB entry (files stay)
install-linux.sh --check         # say what the script sees, change nothing
options: --dry-run, --format (partition mode: mkfs.vfat the partition first),
         --name "Atari" (menu title), --help
```

### 3.1 GRUB entry: `custom.cfg`, nothing else

Upstream GRUB's `41_custom` makes every generated `grub.cfg` source
`${config_directory}/custom.cfg` (or `$prefix/custom.cfg`) **if it
exists**, so a file next to `grub.cfg` adds a menu entry without touching
`grub.cfg` or running `update-grub`/`grub2-mkconfig`. The script:

- finds `grub.cfg` in this order: `/boot/grub/grub.cfg`,
  `/boot/grub2/grub.cfg`, `/boot/efi/EFI/*/grub.cfg` (the first that
  exists and contains the string `custom.cfg`; Fedora's small EFI stub
  `grub.cfg` that only `configfile`s `/boot/grub2/grub.cfg` does not
  count, follow it); if none mentions `custom.cfg`, stop: "this GRUB does
  not read custom.cfg; add the entry below to /etc/grub.d/40_custom and run
  update-grub yourself" and print the entry;
- writes `custom.cfg` **only** if it does not exist or its first line is
  our marker `# atari-os-x86 (install-linux.sh; remove with --remove)`;
  an existing foreign `custom.cfg` is never modified: print the entry to
  add by hand and stop;
- the entry has a fixed id so `grub-reboot atari-os` works where
  `GRUB_DEFAULT=saved` is in use (the script says whether it is, reading
  `/etc/default/grub`; it does not change it). Partition mode:

  ```
  menuentry "Atari" --id atari-os {
      insmod part_msdos
      insmod part_gpt
      insmod fat
      search --no-floppy --fs-uuid --set=root 1A2B-3C4D
      linux /atari-os/vmlinuz <CMDLINE> atari.media=1A2B-3C4D
      initrd /atari-os/initramfs
  }
  ```

  `<CMDLINE>` is the first `cmdline:` of the zip's `limine.conf` (so the
  kernel options stay in one place). Loopback mode:

  ```
  menuentry "Atari" --id atari-os {
      insmod part_msdos
      insmod part_gpt
      insmod ext2          # or btrfs / xfs, from blkid TYPE of the host fs
      insmod loopback
      insmod fat
      search --no-floppy --fs-uuid --set=root <host fs UUID>
      loopback atari /<path of the image relative to that fs>
      linux (atari,msdos1)/atari-os/vmlinuz <CMDLINE> atari.image=<host fs UUID>:/<path>
      initrd (atari,msdos1)/atari-os/initramfs
  }
  ```

  The path relative to the filesystem root: `findmnt -T <file> -o TARGET`
  gives the mount point; strip it. A separate `/boot` is fine (the host fs
  UUID is the one holding the image, not `/boot`). The image must not be
  on btrfs with compression or on an encrypted/LVM volume GRUB cannot open
  (`--check` warns: `lsblk -no TYPE,FSTYPE` of the backing device; `crypt`
  and `lvm` refuse, `btrfs` warns about `chattr +C` and prints it).
- `--remove`: delete `custom.cfg` if its first line is our marker, else
  say it is not ours. Nothing else.
- The menu may be hidden (Ubuntu `GRUB_TIMEOUT_STYLE=hidden` with a single
  OS): the script ends by saying so when it reads that, and that holding
  **Shift** (BIOS) or pressing **Esc** (UEFI) at power-on shows the menu,
  or `sudo grub-reboot atari-os && sudo reboot` once when `GRUB_DEFAULT=saved`.

### 3.2 Targets the script accepts (and refuses)

Partition mode, `--partition /dev/X`: X must be a partition (`lsblk -no
TYPE` = `part`), on any disk, that is **not** mounted, not in
`/etc/fstab` (by name, UUID, PARTUUID or LABEL), not the EFI system
partition (`PARTTYPE` `c12a7328-…` or MBR `ef`), not `crypto_LUKS`,
`LVM2_member`, `linux_raid_member`, `swap` (any of these: refuse), and
≥ 1.5 GiB. Then:

- `blkid TYPE` = `vfat` and the volume is empty or holds only our files
  (`atari-os/`, `atari/`, `EFI/`, `limine*`): copy without formatting;
- `vfat` with other files: refuse unless `--format`;
- any other filesystem or none: refuse unless `--format`.

`--format` runs `mkfs.vfat -F 32 -n ATARI -s 8` **after** the user types
the device name (`/dev/sdXN`) and `yes` at two prompts (as `flash.sh`
does), never when `--dry-run`. Copying: `unzip -o -q` the zip into a
temporary mount (`mount -t vfat -o shortname=mixed`), `sync`, `umount`.
Serial: `blkid -s UUID -o value`. On MBR disks also set the partition type
to `0c` if it is not already a FAT type (`sfdisk --part-type`, after the
same confirmation) so firmware and GRUB see it as expected; GPT: leave the
type alone (basic data works).

Loopback mode, `--image <path>`: the path's directory must exist on an
ext4/btrfs/xfs filesystem (`findmnt -T -o FSTYPE`); free space ≥ image
size + 64 MiB. Copy (or `xz -dc` for `.img.xz`) to that path with a `.part`
suffix, `sync`, rename. The file is only ever created, never overwritten
unless `--force`. If the fs is btrfs, `chattr +C` is applied to the empty
file first (no CoW for a loop image).

### 3.3 Checks before anything is written

- root (`id -u` = 0), else "run with sudo";
- `grub.cfg` found and reads `custom.cfg` (3.1);
- Secure Boot: on EFI systems read
  `/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c`,
  byte 5 (`od -An -tu1 -j4 -N1`); `1` → stop: "Secure Boot is on; turn
  it off in the firmware setup (Limine and the Atari kernel are unsigned)".
  Not EFI → nothing to check;
- tools: `blkid lsblk findmnt unzip mkfs.vfat` (partition mode),
  `xz` (for `.img.xz`); missing → say the package (`dosfstools`, `unzip`,
  `xz-utils`/`xz`).

`--check` runs all of the above, prints the GRUB flavour, the Secure Boot
state, the partitions that would be acceptable (table: device, size,
fstype, label, verdict), and exits 0 without writing.

## 4. `scripts/install-windows.ps1`

PowerShell 5.1 (what every Windows 10/11 has), `#Requires -RunAsAdministrator`,
`Set-StrictMode -Version 2`, `$ErrorActionPreference = 'Stop'`, UEFI only
(BIOS Windows machines use the stick: the script says so and exits).
Usage (`Get-Help .\install-windows.ps1`):

```
.\install-windows.ps1 -Zip atari-os-x86-<v>-files.zip -Partition E:          # an existing empty FAT32 volume
.\install-windows.ps1 -Zip atari-os-x86-<v>-files.zip -ShrinkMB 2048         # shrink C: and make the partition
.\install-windows.ps1 -Remove                                                # drop the firmware entry
.\install-windows.ps1 -Check                                                 # what the script sees
switches: -WhatIf (plan only), -BootNow (one-shot into Atari on the next restart and restart),
          -Menu (also add Atari to the Windows Boot Manager menu, §4.4), -Name "Atari"
```

PowerShell execution policy: the README says to run it as
`powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 …` from an
administrator PowerShell; the script does not change the policy.

### 4.1 Checks

- `$env:firmware_type -eq 'UEFI'` (else exit with the stick hint);
- `Confirm-SecureBootUEFI` → `$true`: stop with the firmware-setup hint
  (the cmdlet throws on non-UEFI/unsupported: treat as unknown, warn,
  continue);
- the zip exists and contains `EFI/BOOT/BOOTX64.EFI` and
  `atari-os/vmlinuz` (`System.IO.Compression.ZipFile` listing);
- BitLocker on C: is fine (shrinking a BitLocker volume is supported);
  say so in `-Check` output.

### 4.2 Target

- `-Partition X:`: an existing volume with `FileSystem -eq 'FAT32'`, not
  the Windows volume, not the system (EFI) partition, not the recovery
  partition (`Get-Partition | Where DriveLetter`, `.IsSystem`,
  `.Type`), size ≥ 1.5 GB, and empty or holding only our folders; else
  refuse ("format it as FAT32 in Disk Management first, or use -ShrinkMB").
  The script never formats an existing volume.
- `-ShrinkMB n` (n ≥ 1536): `Get-PartitionSupportedSize` for C: must allow
  it; print the plan (`C:` from a GB to b GB, new partition n MB); the user
  types `yes`; then `Resize-Partition`, `New-Partition -UseMaximumSize
  -AssignDriveLetter` on the same disk (GPT: basic data type; MBR: the
  default), `Format-Volume -FileSystem FAT32 -NewFileSystemLabel ATARI
  -AllocationUnitSize 4096`. Any failure after the shrink: say exactly what
  happened and that C: can be extended back in Disk Management; do not
  try to undo automatically.
- Copy: `Expand-Archive -Force` into `X:\`. Then add
  `atari.media=<serial>` to every `cmdline:` line of `X:\limine.conf`
  (serial from `(Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='X:'").VolumeSerialNumber`,
  `AAAA-BBBB` form), exactly what init's installer does.

### 4.3 Firmware entry (the one firmware change)

```
$guid = bcdedit /copy "{bootmgr}" /d "Atari"      -> parse {GUID}
bcdedit /set $guid device partition=X:
bcdedit /set $guid path \EFI\BOOT\BOOTX64.EFI
bcdedit /set "{fwbootmgr}" displayorder $guid /addlast
```

`/addlast` appends to the firmware's boot list so the entry shows in the
one-time boot menu (F12/Esc/F9…); the default stays Windows. An entry
with description `Atari` (or `-Name`) whose path is ours is reused, not
duplicated (`bcdedit /enum firmware` parse). `-BootNow`:
`bcdedit /set "{fwbootmgr}" bootsequence $guid` (one shot, like our F8
"Reboot straight back into Atari") then `Restart-Computer -Confirm`.
`-Remove`: `bcdedit /delete $guid` for our entry (and the `-Menu` entry if
present); files untouched. All `bcdedit` output is checked for "The
operation completed successfully" and failures are reported verbatim.

### 4.4 `-Menu` (optional, off by default)

Adds Atari to the **Windows Boot Manager** menu instead of (or as well as)
the firmware list, for machines whose firmware hides extra entries:

```
$g = bcdedit /create /d "Atari" /application bootapp
bcdedit /set $g device partition=X:
bcdedit /set $g path \EFI\BOOT\BOOTX64.EFI
bcdedit /displayorder $g /addlast
bcdedit /timeout 10      # only if the current timeout is 0 or the menu was not shown; say so
```

This shows the blue Windows chooser at every start; the script says that
and that `-Remove` takes it away (restoring `timeout` to what it was,
stored in the entry's description tail `Atari [t=0]` or a sidecar
`X:\atari-os\windows-install.json`; pick one, document it).

## 5. init: `atari.image=<UUID>:<path>` (loopback mode)

`init/init.c`:

- parse `atari.image=` (UUID up to 40 chars, path up to 255) next to
  `atari.media=`; comment block at the top of the file documents it;
- in `scan_block_devices()`, when set: before everything else, look for
  the partition with that filesystem UUID (`blkid -s UUID -o value` from
  busybox, one fork per candidate is acceptable here; or read ext4's UUID
  at superblock offset 0x468 directly: implementer's choice, say which),
  mount it on `HOSTFS` (`/.host`) **read-write** with the type busybox
  blkid reports (`ext4`, `btrfs`, `xfs`; `vfat`/`exfat`/`ntfs3` too), then
  `attach_loop(path, partscan, writable)` and mount `p1` as today in
  `try_image_file()` (refactor so both share the loop+mount part);
  `media_kind` = `image-file`. Like `atari.media`, for the first 5 s
  nothing else is tried;
- `scripts/mkimage.sh` `INITRAMFS_MODULES` gains `ext4` (with its deps:
  `resolve_mods` does that), `xfs`, `btrfs`; report the initramfs size
  before/after (budget: +3 MiB compressed; if btrfs alone is more than
  that, leave btrfs out and say so in README);
- `status` already reports `media image-file`; the hardware page shows
  the host device and path (new `status` line `image <dev> <path>` when
  loopback mode is active: sysmenu prints it after "Boot medium").

## 6. Verification

Linux script (as the agent, no root on the host: everything that writes
runs inside QEMU, where we are root, or in `--dry-run`/`--check` mode):

- `shellcheck scripts/install-linux.sh`; `--help`, `--check` on this
  machine (it has GRUB), `--dry-run --partition /dev/<some existing
  partition>` prints the plan and the refusal reasons (mounted, fstab);
- a **host-Linux test disk**: `scripts/mk-hostfs-test.sh` makes
  `build/hostfs-test.img` (4 GiB sparse): an ext4 filesystem populated
  with `mkfs.ext4 -d` from a directory holding `atari/atari-os-x86.img`
  (the current image; e2fsprogs on the host or `e2fsprogs` in
  `BUILD_PKGS`), **no partition table** needed for the init test;
- **init loopback test**: `scripts/qemu-test.sh` gets `QEMU_KERNEL=1`:
  boot with `-kernel build/stick/atari-os/vmlinuz -initrd
  build/stick/atari-os/initramfs -append "<CMDLINE> console=ttyS0,115200
  atari.debug=1 atari.image=<uuid>:/atari/atari-os-x86.img"` and the
  hostfs disk as a non-removable drive (`QEMU_DISK`), no stick. Expect:
  atari800 runs, `status` says `media image-file`, serial `mount` shows
  `/.host` (ext4, rw) and `/media` (vfat on `loop0p1`), writing a file
  through ASH or `touch /media/atari/x` lands inside the image (check with
  `mdir` on the host after the run). Screenshot of the hardware page with
  the `image` line;
- **GRUB test** (the real thing, best effort): build
  `build/grub-test.img` with a BIOS GRUB that has our `custom.cfg`. Inside
  QEMU we are root but have no grub tools; on the host there is no root.
  Use the Alpine env: `apk add grub grub-bios` into the chroot and
  `grub-mkimage`/`grub-mkrescue`-style assembly without loop devices:
  `grub-mkstandalone -O i386-pc --output=core.img "boot/grub/grub.cfg=…"`
  then `cat lnxboot.img core.img > build/grub-test/boot.bin` and make a
  disk with `mformat`/`mcopy` + `sfdisk` on the image file + `dd` of
  boot.bin into the gap; boot it with `QEMU_IMG=build/grub-test.img
  QEMU_IMG_AS=ide`, with the custom.cfg entry in **partition mode**
  pointing at a FAT partition on the same disk (second partition, filled
  by `mcopy` from the zip) and, in a second run, in **loopback mode**
  pointing at the hostfs disk. If grub-mkstandalone in the chroot turns
  out impossible within an hour, stop, document the exact blocker and
  test the generated `custom.cfg` text against GRUB's syntax by eye; say
  so in the report;
- `--remove` leaves no `custom.cfg` when ours; a foreign one is left
  alone (unit test with `--root <dir>` pointing the script at a fake
  `/boot`: add that hidden option for tests, documented in the header).

Windows script: no Windows here. Required: `pwsh` parse check if
`pwsh` can be installed (`apk add powershell` is not in Alpine; try the
Microsoft tarball into `build/pwsh/` on the host; if that fails, skip and
say so), a careful read of every cmdlet against its PowerShell 5.1
signature, and **`-WhatIf`/`-Check` paths written so they can run on a
machine without admin** for the first user test. README marks it
"tested on Windows 11 by: nobody yet" until Carlos or a user runs it.

## 7. Docs

README "Install" gets a **"Next to Linux or Windows (dual boot)"**
subsection: the three ways (Linux partition, Linux loopback, Windows
partition), the exact commands, the Shift/Esc hidden-menu note, the
Secure Boot requirement, what `--remove`/`-Remove` does, and the line
that the Windows script appends one firmware entry and never changes the
default. KNOWN-ISSUES: Windows script untested; GRUB without `41_custom`;
btrfs compression and loopback; the Windows chooser (`-Menu`) shows at
every start. CHANGELOG line. `release-template.md` "Dual boot" paragraph
pointing at the zip and the two scripts.
