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
