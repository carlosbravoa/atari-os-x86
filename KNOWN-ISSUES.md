# Known issues

- **Wi-Fi passwords from the F8 System menu**: the field takes up to 79
  characters, but WPA2 passphrases must be 8-63 characters (64 hex digits
  are taken as a raw key); anything else is now refused with "password
  rejected by wpa_supplicant (8-63 characters)". The Atari keyboard has no
  keys for `{ } ~` and backtick, so passwords containing them cannot be
  typed (the prompt says so). After a connect the menu waits up to 20 s and
  reports wpa_supplicant's verdict: "wrong password", "connection failed"
  or "network not found" (the `wifierr` line of init's `status` reply).
  Where to look: `atari/logs/` on the stick ("Save logs to the stick":
  `wpa.log` has wpa_supplicant's events, `dmesg.txt` init's `wifi:` lines
  with the SSID and password length, never the password). Workaround for
  passwords the menu cannot type: add the network to
  `atari/config/wpa_supplicant.conf` on the stick by hand.
- **Keyboard**: in the PC keyboard type (the default) `{ } ~` cannot be
  typed (the Atari has no such keys; Shift+`[` `]` give `[ ]`, and
  backtick is the Atari/inverse key). The Atari keyboard type (F8 >
  Controls) is positional for the symbol keys only, after a US layout.
  The Alt+letter shortcuts shown in atari800's own menu (F8 > Advanced)
  are not bound, and never were. The Help page names the Atari key "Win /
  key left of 1": the Atari font has no backtick to print.
- **Input lag figures** (Hardware page, `-kms-debug`): from the kernel's
  time stamp of the event to the vblank the frame was flipped on. Not in
  them: the USB poll interval before the event (1 ms with the new kernel
  options, the device's own scan time unknown) and the panel's scan-out
  and pixel response time (often 5-15 ms more on laptop panels). With
  Low latency on, the CPU runs at full performance preference and the
  fan may be heard more; switch it off in F8 > Controls. In QEMU the
  numbers are indicative only (no real panel). The display waits only
  apply when the display paces the emulator (60 Hz panel, NTSC).
- **Old i3 (Broadwell) laptop**: blurry picture (firmware framebuffer, i915
  probably not taking over) and a firmware "self test" screen after one
  boot. No logs captured yet.
- **Unclean stops** (power cut, the stick pulled, a hang): lost clusters
  and a wrong free count on the stick's FAT used to pile up until copies
  failed with "No space left" (one stick had 828 MB of them). init now
  runs `fsck.fat -a` before mounting the stick (and every FAT USB drive),
  so they are reclaimed at the next boot. A stick pulled while a file is
  being written can still lose that file (fsck.fat reclaims its clusters
  as free space, it does not bring the file back).
- **Lid switch** does nothing yet (volume/brightness keys work).
- **ZenBook UX425EA reports its battery as absent** (ACPI `BAT0` absent at
  boot), so the menu shows no battery level there. Not investigated.
- **Clock**: laptops with a dead CMOS cell boot in 2016; init sets the time
  from the network once DHCP is up and writes it back to the RTC, but until
  then TLS (HTTPS hosts in FujiNet) fails.
- **Module archive** (modloop) is 194 MB after dropping Nvidia GSP and old
  Intel Wi-Fi firmware and the driver classes below; AMD GPU firmware (26 MB)
  is kept whole, per-GPU trimming is possible later.
- **Drivers left out of the modloop** (to keep it small): no TV tuners,
  DVB or capture cards, no FC/SAS/iSCSI host adapters or hardware RAID,
  no cluster or network filesystems (NFS, Ceph, GFS2, ...), no datacenter
  NICs (Mellanox, Intel ice/i40e/ixgbe, Broadcom bnxt, ...), no
  InfiniBand, device-mapper/MD RAID, netfilter, IIO sensors
  (accelerometers, ambient light; also the Cherry Trail AXP288 battery
  gauge) or NFC. See "Building" in the README to put a class back.
- **Installing onto the laptop's disk** (F8): Secure Boot must be off on
  UEFI machines (Limine is unsigned), as for the stick. The installer
  makes exactly one partition over the whole disk and erases everything
  else; there is no dual boot from the menu (that is what
  `install-linux.sh` and `install-windows.ps1` are for). The partition
  table is MBR, so on disks over 2 TiB only the first 2 TiB are used.
  Some UEFI firmware ignores or drops boot entries written by the OS; such
  a laptop still boots the disk through the removable-media path
  (`\EFI\BOOT\BOOTX64.EFI`) once nothing else comes first in its boot
  order. Tested in QEMU (SeaBIOS, OVMF; SATA, NVMe, virtio), not yet on a
  real laptop.
- **Dual boot** (`install-linux.sh`, `install-windows.ps1`):
  - The **Windows script has not been run on Windows yet** (written against
    PowerShell 5.1's cmdlets, but untested; there was no PowerShell to even
    parse-check it). `-Check` and `-WhatIf` are safe first steps and work
    without administrator rights. `bcdedit`'s messages are localised, so the
    script goes by its exit codes and parses only `{GUID}`s and the
    `displayorder`/`timeout`/`path`/`description` lines.
  - `-Menu` (the Windows Boot Manager entry) shows the blue chooser at every
    start, and Boot Manager may refuse to start Limine (an unsigned,
    non-Windows loader); the firmware's own boot menu is the reliable way.
  - Windows **Fast Startup** keeps the ATARI drive mounted in its hibernated
    session: files changed on it from both sides between two starts can be
    lost. Turn Fast Startup off, or change the files from one side only.
  - Some UEFI firmware ignores or drops boot entries an OS adds (as for the
    whole-disk installer); then the Atari is not in the boot menu. `-Menu`
    is the fallback there.
  - A **GRUB without `41_custom`** (hand-written `grub.cfg`, some
    distributions' custom setups) does not read `custom.cfg`: the Linux
    script then installs the files and prints the entry to add by hand
    (exit status 2). Tested with upstream GRUB 2.14 (BIOS and UEFI) in QEMU,
    both modes; not yet with a distribution's signed EFI GRUB (shim, Secure
    Boot off) on real hardware.
  - **Loopback mode on btrfs**: the image needs `chattr +C` (no
    copy-on-write, no compression), which the script applies to the new
    file; an image copied there by hand on a compressed btrfs cannot be
    read by GRUB. Subvolume layouts are handled through the mount's
    FSROOT (Ubuntu's `@`, Fedora's `root`), but a btrfs whose default
    subvolume is not the top level (openSUSE snapshots) is not supported:
    init mounts the default subvolume. Tested in QEMU on ext4 and btrfs;
    xfs is built in but untested.
  - Loopback mode mounts the host's filesystem **read-write**: never start
    the Atari from a Linux that is hibernated. LVM and LUKS volumes are
    refused (init cannot open them).
  - The loopback filesystems (ext4, xfs, btrfs) add 2.4 MB to the
    initramfs (kept as `.ko.xz` there; 3.4 MB as `.ko.gz`).
- **Local disks with Boot into: FujiNet** load at standard SIO speed
  (19200 baud, like a real 810: ~20 s for a 90 KB boot): NetSIO turns
  atari800's SIO patch off, so a local D1: answers on the emulated serial
  line, where FujiNet's own disks get its high-speed SIO.
- **No BASIC (self test)** shows the XL self test only with real XL ROMs in
  `atari/roms/`; the built-in AltirraOS shows its own boot screen instead
  (it has no self test).
- **NTSC TV look**: the filter starts each line from black, so the first
  few panel columns of the picture are darker, and the picture sits a
  pixel or two to the right (atari800's SDL port does the same).

## ASH: copying or saving to h: fails (ASH 1.3 TN bug)

`cp … h:name`, `mv … h:name` and the editor's save on `h:` fail with an I/O
error (129). Cause: in ASH's `fileops.s` `w_open`, the TN build's `bne wo_d`
after the `sd:` check tests the flags left by `cmp #5`, not the drive, so
an `h:` destination takes the disk (CIO) path. Fix belongs in ASH: `lda
wdev` before `bne wo_d`; then rebuild `release/ASH-TN.*`/`ASH.CAR` and bump
the submodule. Reads from `h:`, `ls`, `cat`, `run`, `cp h: → sd:`/disk,
`rm`/`mv` on `h:` and everything on `sd:` work.
