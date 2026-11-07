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

- **Windows**: [balenaEtcher](https://etcher.balena.io/) takes the
  `.img.xz` as it is. [Rufus](https://rufus.ie/) also works: unpack it
  first (7-Zip) and write it in **DD mode**.
- **Linux and macOS**: `flash.sh` (attached) lists only USB sticks and SD
  cards, asks you to type the device name and `yes`, writes and reads the
  stick back to verify it:
  `bash flash.sh atari-os-x86-@TAG@.img.xz`
- **Ventoy**: unpack it (`unxz atari-os-x86-@TAG@.img.xz`) and copy the
  `.img` onto your Ventoy stick like any other image.

`SHA256SUMS` has the checksums (`sha256sum -c SHA256SUMS`).

Then start the computer from the stick (boot menu key: usually F12; F9 on
HP, Esc or F8 on ASUS). **F8** opens the System menu (Wi-Fi, boot into
BASIC/ASH/FujiNet, shut down), **F9** the Library.

### Hardware

- 64-bit x86 (x86_64) PC or laptop; 32-bit-only machines are not supported yet
- BIOS or UEFI boot; on UEFI, Secure Boot must be off
- 1 GB RAM (what it is tested with)
- a USB stick or SD card of 1 GB or more
- Intel, AMD or NVIDIA graphics, or any firmware framebuffer

Known problems: [KNOWN-ISSUES.md](https://github.com/@REPO@/blob/@TAG@/KNOWN-ISSUES.md).
