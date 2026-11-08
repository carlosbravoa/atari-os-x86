# Changelog

All notable changes to atari-os-x86. Versions are git tags (`v0.1.0`); the
image, the boot splash, the System menu's hardware page and init's `status`
reply all show the version they were built from.

## Unreleased

- Boots x86_64 PCs and laptops straight into atari800 from one FAT32 USB stick, BIOS and UEFI (Limine)
- Own PID 1 (`init/init.c`): driver loading by modalias without udev, modloop with the full module and firmware set, hotplug via uevents
- atari800 port without SDL: DRM/KMS video with vsync page flips, ALSA sound, evdev input with hotplug, USB gamepads
- Boot splash with progress lines and an "F8 = System menu" hint for the first boots
- F8 System menu: boot into BASIC, ASH or FujiNet CONFIG; volume and brightness; Wi-Fi scan and connect with diagnostics; SSH on/off and password; hardware status page; save logs to the stick; restart, reboot, shut down
- Reboot straight back into the Atari through UEFI `BootNext`, without ever changing the machine's boot order
- F9 Library: browse, run, mount and copy Atari files across the stick, USB drives and FujiNet's SD folder; recent files, favourites, filter, zip files, multi-disk sets
- USB drives mounted under `/media/usb` for the Library and ASH
- FujiNet-PC built for musl and started next to the emulator over NetSIO; uses the laptop's Ethernet or Wi-Fi; web UI on port 8000
- ASH, the Atari shell, on the stick (cartridge, program, boot disk), with the Tang Nano's SIO $72 file device emulated: `h:` (atari/hdd) and `sd:` (stick and USB drives)
- Ventoy and other image-file boots: init finds `atari-os*.img` on any partition and loop-mounts it as the stick
- Remote access over SSH (dropbear), keys on the stick, off by default
- Network time after DHCP, written back to the RTC
- Headless QEMU test harness (`scripts/qemu-test.sh`) and an unprivileged Alpine build environment (`scripts/alpine-env.sh`)
- Versioned images (`atari-os-x86-<version>.img`), version on the splash, hardware page and `status` reply
- Release pipeline: GitHub Actions build, host `-Werror` build and QEMU smoke boot on every push; releases on `v*` tags
- `scripts/flash.sh`: a safe stick writer for Linux and macOS (removable disks only, type the name, verify by read-back)
- Install onto the laptop's own disk from the F8 menu (whole disk, type ERASE): one FAT32 partition, boots on BIOS and UEFI (firmware entry "Atari" first in the boot order); `atari.media=<serial>` makes the installed copy win over a stick left plugged in, and a booted stick wins over an installed copy
- Every route boots what the user asked for: what is in D1: (or a program, cartridge, tape) boots on any cold start, with ASH's cartridge out and BASIC off for that boot; the boot target (BASIC, No BASIC, ASH, FujiNet CONFIG) applies when nothing is mounted; F8 and the Library "Eject all" go back to it; Library "Run with BASIC"; with FujiNet a local D1: wins over FujiNet's drive
- The Library's first tab is the whole stick (opening in `library/`; FujiNet's SD is `Stick: /fujinet/SD`); Copy to Library, FujiNet SD or a USB drive
- F8 Display: Integer/Fit/Fill scaling, sharp-bilinear smoothing, scanlines, border width, NTSC TV look (atari800's NTSC filter)
