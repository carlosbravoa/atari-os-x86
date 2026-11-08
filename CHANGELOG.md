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
- Dual boot next to Linux or Windows: `atari-os-x86-<version>-files.zip` (the stick's files) in every build and release; `scripts/install-linux.sh` adds a GRUB entry through `custom.cfg` only (FAT32 partition, or the image as a file on ext4/btrfs/xfs with GRUB `loopback` and the new `atari.image=<UUID>:<path>`); `scripts/install-windows.ps1` (UEFI) puts the files on a FAT32 partition (existing, or made by shrinking C:) and appends one firmware boot entry, never changing the default; both check Secure Boot, show a plan, ask before writing, and `--remove`/`-Remove` undo the entry
- init: `atari.image=` loopback mode (host filesystem read-write on `/.host`, `status` line `image <dev> <path>`, shown on the hardware page); ext4, xfs and btrfs in the initramfs; at shutdown the modloop and the image's loop device are released first, so `/media` and a host filesystem unmount cleanly
- F1 = Help: every key one per line in a scrolling list (F1-F12 in order, Atari keys, keyboard type with the positional table when it is Atari, joystick, gamepad, Library, menus, power off); the boot hint reads "F1 = Help   F8 = System   F9 = Library" until one of them is opened. atari800's own menu moves to F8 > "Advanced: emulator menu..." (its F8 entry, "System menu", comes back)
- F8 System menu regrouped: Library, Boot into, Eject all, Display..., Sound, Controls..., Network..., Help, Hardware status, Save logs, Restart, Reboot, Shut down, Advanced; TV system and Brightness move into Display, Wi-Fi/IP/SSH into Network; the gamepad's Guide button opens it
- F8 Controls: arrows as cursor keys or joystick 0, keyboard type PC (keys type their caps) or Atari (positional: Shift+2 = `"`, `-` = `<`, `[` = `-`, `'` = `+`, ...), gamepad 1 on joystick 0 or 1, gamepad buttons page (`KMS_KEYBOARD`, `KMS_PAD_SWAP`)
- Shift+arrow keys are cursor keys too (they typed `- = + *`)
- F8 "Report a problem...": a one-line description plus version, machine, settings, mounted media and the recent emulator/FujiNet logs written to `atari/logs/report-<MMDD-HHMMSS>.txt` on the stick (dmesg and full logs next to it), to attach to a GitHub issue
- F1, F8 and F9 toggle: inside a menu the same key closes it (all levels), another of the three switches to its menu
- F9 Library: a Drives line (what is in D1:-D4:, the cartridge and the tape) opening a popup to eject one of them or all; "Mount as D1:" to "D4:" for disk images
