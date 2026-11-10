## atari-os-x86 @TAG@

`atari-os-x86-@TAG@.img.xz` is a complete USB stick image (1 GiB, xz
compressed): it boots an x86 PC or laptop straight into an Atari 8-bit
computer (the atari800 emulator, with FujiNet-PC and the ASH shell). Nothing
is installed on the computer; everything lives on the stick, including your
Atari files, which you can add from any other computer (the stick stays a
normal FAT32 drive). The image uses the first 1 GiB of the stick; the rest
stays unused for now.

### Install

Pick one (the stick is erased):

- **Windows**: [balenaEtcher](https://etcher.balena.io/): *Flash from
  file* → the `.img.xz` (no unpacking) → *Select target* → your stick →
  *Flash!*. Click **Cancel** if Windows offers to format or scan the
  stick afterwards; it then appears as a drive called **ATARI**, and you
  can drop games into `atari\library` before ejecting it safely.
  [Rufus](https://rufus.ie/) also works in **DD Image** mode (unpack with
  7-Zip first if it does not take the `.xz`). Step by step: README,
  "Writing the stick on Windows".
- **Linux and macOS**: `flash.sh` (attached) lists only USB sticks and SD
  cards, asks you to type the device name and `yes`, writes and reads the
  stick back to verify it:
  `bash flash.sh atari-os-x86-@TAG@.img.xz`
- **Ventoy**: unpack it (`unxz atari-os-x86-@TAG@.img.xz`) and copy the
  `.img` onto your Ventoy stick like any other image.

Machines without a 64-bit CPU (Core Duo, Pentium M, Atom N270 netbooks)
need the experimental 32-bit image, `atari-os-x86-@TAG@-i686.img.xz`,
written the same way (its stick files: `atari-os-x86-@TAG@-i686-files.zip`).
It is tested in QEMU only, not yet on a real machine: if you try it, please
open an issue saying which laptop and what happened (README, "32-bit
machines").

`SHA256SUMS` has the checksums (`sha256sum -c SHA256SUMS`).

### Dual boot

To start the Atari from a computer's own boot menu next to Linux or Windows
instead of from a stick, use `atari-os-x86-@TAG@-files.zip` (the stick's
files) with one of the attached scripts (Secure Boot must be off; both show
their plan with `--dry-run` / `-WhatIf` and ask before writing anything):

- **Linux** (GRUB): `sudo sh install-linux.sh --partition /dev/sdXN atari-os-x86-@TAG@-files.zip`
  puts the files on a FAT32 partition of 1.5 GiB or more, or
  `sudo sh install-linux.sh --image /home/atari.img atari-os-x86-@TAG@.img.xz`
  keeps the whole image as a file on your Linux partition. The menu entry
  "Atari" goes into GRUB's `custom.cfg`; `grub.cfg` and the default entry
  are not touched. `--remove` takes the entry away again.
- **Windows** (UEFI), in an administrator PowerShell:
  `powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Zip atari-os-x86-@TAG@-files.zip -ShrinkMB 2048`
  (or `-Partition E:` for an empty FAT32 volume). It adds one firmware boot
  entry "Atari" at the end of the boot list, so it is in the boot menu (F12,
  F9, Esc ...); Windows stays the default. `-Remove` takes it away again.
  Not yet tested on a real Windows machine: start with `-Check`.

Details: README, "Next to Linux or Windows (dual boot)".

Then start the computer from the stick (boot menu key: usually F12; F9 on
HP, Esc or F8 on ASUS). **F1** shows every key, **F8** opens the System
menu (Wi-Fi, boot into BASIC/ASH/FujiNet, shut down), **F9** the Library.

### Hardware

- 64-bit x86 (x86_64) PC or laptop; 32-bit-only machines (Core Duo, Pentium M, Atom N270): the experimental i686 image, untested on real hardware
- BIOS or UEFI boot; on UEFI, Secure Boot must be off
- 1 GB RAM (what it is tested with)
- a USB stick or SD card of 1 GB or more
- Intel, AMD or NVIDIA graphics, or any firmware framebuffer

Known problems: [KNOWN-ISSUES.md](https://github.com/@REPO@/blob/@TAG@/KNOWN-ISSUES.md).
