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
- **Old i3 (Broadwell) laptop**: blurry picture (firmware framebuffer, i915
  probably not taking over) and a firmware "self test" screen after one
  boot. No logs captured yet.
- **Lid switch** does nothing yet (volume/brightness keys work).
- **ZenBook UX425EA reports its battery as absent** (ACPI `BAT0` absent at
  boot), so the menu shows no battery level there. Not investigated.
- **Clock**: laptops with a dead CMOS cell boot in 2016; init sets the time
  from the network once DHCP is up and writes it back to the RTC, but until
  then TLS (HTTPS hosts in FujiNet) fails.
- **amdgpu firmware** makes the modloop ~400 MB; per-GPU trimming is
  pending.
- **Installing onto the laptop's disk** (F8): Secure Boot must be off on
  UEFI machines (Limine is unsigned), as for the stick. The installer
  makes exactly one partition over the whole disk and erases everything
  else; there is no dual boot from the menu (a Linux-host script and a
  Windows installer for that are separate, later work). The partition
  table is MBR, so on disks over 2 TiB only the first 2 TiB are used.
  Some UEFI firmware ignores or drops boot entries written by the OS; such
  a laptop still boots the disk through the removable-media path
  (`\EFI\BOOT\BOOTX64.EFI`) once nothing else comes first in its boot
  order. Tested in QEMU (SeaBIOS, OVMF; SATA, NVMe, virtio), not yet on a
  real laptop.
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
