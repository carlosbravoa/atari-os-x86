# Known issues

- **Long Wi-Fi passwords cannot be entered** in the F8 System menu
  (reported on the first laptop test; the password field in the Atari-style
  text editor appears to cap the length). Workaround: add the network to
  `atari/config/wpa_supplicant.conf` on the stick by hand, or use a shorter
  passphrase. To investigate: `fEditString` width limits in
  `ui_basic.c` versus the 80-byte buffer in `emulator/kms/sysmenu.c`.
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
