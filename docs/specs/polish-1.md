# Spec: polish round 1 — FujiNet Wi-Fi bridge, zip in the Library, Wi-Fi diagnostics

Status: agreed 2026-11-07 (Carlos: "anything we could do in the meantime?";
the FujiNet route was his question). Design: Claude (Fable); implementer: an
Opus agent; review, QEMU/laptop verification and commits: Claude.

Three independent pieces. Do them in the order A, B, C; each must build and
be verified on its own. Same rules as `docs/specs/library.md` §3 and §7:
C89 `-ansi -pedantic -Werror` for everything under `emulator/` and `init/`,
musl-clean, no new libraries, `Log_print`/`logmsg` for diagnostics, nothing
else may map a single key to `AKEY_EXIT`. Do not commit. Report at the end:
files changed, what was verified with which screenshots/log lines, anything
in this spec that was wrong or impossible and what you did instead.

Build env reminders: `scripts/alpine-env.sh run <cmd>` runs inside the
Alpine root (project at `/work`). Emulator: `scripts/alpine-env.sh run
scripts/build-emulator.sh`; host warning check: `cd build/atari800-host &&
make -j16`. FujiNet-PC: `scripts/alpine-env.sh run scripts/build-fujinet.sh`
(patches in `fujinet/patches/*.patch` are applied to the submodule
`third_party/fujinet-firmware` before building; add a new patch file, do
not edit the submodule in place without producing the patch with
`git -C third_party/fujinet-firmware diff`). Image:
`scripts/alpine-env.sh run sh -c 'SKIP_STAGE=1 EXTRA_CMDLINE="console=ttyS0,115200 atari.debug=1" scripts/mkimage.sh'`
and wait for the final "Done" line. QEMU: `scripts/qemu-test.sh start|key|shot|serial|log|stop`
(see README "Headless testing"); KVM is available, a boot takes ~15 s;
wait for `pidof atari800` over serial before sending keys.

## A. FujiNet-PC uses the laptop's network, and CONFIG's Wi-Fi screens drive it

### Today

FujiNet-PC's Wi-Fi layer on PC is `lib/hardware/fnDummyWiFi.cpp`
(`DummyWiFiManager`, selected by `fnWiFi.h` when `ESP_PLATFORM` is not
defined). It is a stub: `connected()` is always true after `start()`,
`scan_networks()` returns fake "Dummy Cafe N" entries, `connect()` does
nothing. All real traffic (TNFS, N:, web UI) goes through the host's
sockets, so FujiNet already uses whatever network Linux has (Wi-Fi via
wpa_supplicant, or Ethernet). The consequences on our appliance: FujiNet
CONFIG believes it is online even when the laptop has no network, and the
CONFIG Wi-Fi screens (the way every FujiNet user knows how to set up Wi-Fi)
show nonsense.

### Wanted

When the appliance's control socket `/run/atari.ctl` exists, the dummy
manager becomes a client of it so that FujiNet CONFIG behaves like a real
FujiNet:

- `connected()`: true iff the appliance has an IP address (Wi-Fi *or*
  Ethernet). Cache the answer for 2 s (CONFIG polls it).
- `get_current_ssid()`: the connected SSID; `"Ethernet"` when online
  without Wi-Fi; `""` when offline. `get_current_bssid*()`: keep dummy.
- `get_mac()`/`get_mac_str()`: MAC of the interface that has the IP
  (`/sys/class/net/<iface>/address`); if unknown keep the dummy value.
- `scan_networks(max)`: send `wifi scan`, wait 4 s, send `wifi results`,
  parse the `net <signal_dBm> <wpa|wep|open> <ssid…>` lines (ssid may
  contain spaces; it is the rest of the line), de-duplicate by SSID keeping
  the strongest, sort strongest first, keep at most `max`. Store them for
  `get_scan_result(index, ssid, rssi, channel, bssid, encryption)`: rssi =
  `(uint8_t)(int8_t)signal`, channel 0, bssid dummy, encryption 3 for wpa,
  1 for wep, 0 for open (ESP `wifi_auth_mode_t` values: OPEN 0, WEP 1,
  WPA2_PSK 3). "ERR no wifi adapter" → 0 results.
- `connect(ssid, password)`: send `wifi connect\t<ssid>\t<password>`
  (one line, tab-separated, exactly as `emulator/kms/sysmenu.c` does);
  then poll `status` once a second for up to 25 s until a `wifi connected`
  line and an `ip` line are present → return 0; otherwise return -1. A
  reply starting with `ERR` returns -1 immediately. Keep `_connected`
  consistent with the result.
- `connect()` (no arguments, called at start-up from `src/main.cpp`): do
  not touch `Config`'s stored SSID/passphrase; return `connected() ? 0 : -1`.
- `store_wifi()`: keep as no-op (init saves wpa_supplicant's config).
- `test_connect()`: same as `connect(ssid, password)`.
- Without the socket (developer running FujiNet-PC on a desktop) behaviour
  is exactly as before. Decide once at `start()` (socket path exists) and
  log which mode is active with `Debug_printf`.

Control-socket client: `AF_UNIX` `SOCK_STREAM`, write the command plus
`\n`, read until EOF (init closes after answering; check `ctl_handle()` and
`reply()` in `init/init.c` and the client `ctl_request()` in
`emulator/kms/sysmenu.c` for the exact framing), 3 s receive timeout.
Put it in the same file as a static helper; C++17 is fine there.

Delivered as `fujinet/patches/0002-wifi-bridge-atari-ctl.patch` (only
`lib/hardware/fnDummyWiFi.cpp` and, if a member is needed, `fnDummyWiFi.h`).
Mention the bridge in README.md "FujiNet" (two sentences: FujiNet uses the
laptop's network; CONFIG's Wi-Fi screens configure the laptop's Wi-Fi, same
as F8).

Status reply format (from `cmd_status()` in init): lines such as
`ip 192.168.0.221`, `wifi connected wifly`, `wifi SCANNING wifly`,
`wifi not connected`, `usb 1`, ending with `OK`. Read `cmd_status()` and
`cmd_wifi_results()` before writing the parser.

### Verification (QEMU has an Ethernet NIC and no Wi-Fi)

1. `scripts/qemu-test.sh start` with FujiNet boot on (set `fujinet=1` in
   `atari/config/system.conf` on the test image copy, or use the F8 menu):
   CONFIG must come up on the hosts screen as before (online via Ethernet).
   `/run/fujinet.log` (or wherever init puts FujiNet's output; see
   `init/init.c`) shows the bridge mode line. Screenshot.
2. Add `QEMU_NONET=1` to `scripts/qemu-test.sh`: replaces the `-netdev
   user…`/`-device e1000e` pair with `-nic none`. Boot with it: CONFIG
   must show its Wi-Fi network screen (status 6, "not connected") with an
   empty list instead of pretending to be online. Screenshot. (Real scan
   and connect are verified by Claude on the laptop.)

## B. Zip files in the Library

atari800 has no zip support (zlib only gives `.atr.gz`/`.xfd.gz`). Many
collections are zipped, so the Library handles `.zip` itself, in
`emulator/kms/library.c` (plus a small new `emulator/kms/zipfile.c`/`.h`,
added to the Makefile.am patch in `emulator/patches/`), using zlib's raw
inflate (`inflateInit2(&z, -MAX_WBITS)`). No new libraries.

- Minimal reader: locate the end-of-central-directory record (search the
  last 66 KB), walk the central directory, support methods 0 (stored) and
  8 (deflate), no zip64, no encryption, no data descriptors needed (sizes
  come from the central directory). Validate offsets against the file
  size. Entries larger than 16 MB are skipped.
- RETURN on a `.zip` in the Library lists its *media* entries (same
  extension list as the Library, nested paths shown with their directory
  part, directories themselves skipped). Exactly one media entry → treat
  it as "the file"; several → a popup to pick one; none → `No Atari files
  in this zip`.
- Extraction goes to `/run/atari-zip/` (tmpfs; fall back to `$TMPDIR` or
  `/tmp` when not writable), which is emptied before each extraction. The
  extracted path then flows through the existing action popup exactly like
  a regular file (Run, Mount/Insert, Favourite). Multi-disk detection
  (`library.md` §4.2) runs over the extracted set: when the chosen entry
  matches the disk-1 patterns, extract its siblings too so the sibling scan
  finds them.
- `Copy to …` on a zip copies the zip file itself, unchanged (the archive
  is the collection item).
- Recent/favourites store `zip-path!entry-name` for zipped runs (`!` cannot
  appear in FAT names) and extract again on use; show them as
  `name.zip!ENTRY.XEX`.
- Errors show `Could not open zip: <reason>` via `fMessage`.

Verification: add `GAMES/PACK.ZIP` (two media files in a folder, one
stored and one deflated, made with `zip -0` and `zip -9`, or Python's
`zipfile` run with `python3 -I`) and `GAMES/MULTI.ZIP` (`X (Disk 1).atr`,
`X (Disk 2).atr`) to `build/usb-test.img` via `scripts/mk-usb-test.sh`.
Screenshots: the entry picker for PACK.ZIP, the action popup for the
chosen entry, the emulator after Run; log lines `library: zip …` and
`SIO: mounted D2:` (or the equivalent `Log_print` text) for MULTI.ZIP.
Also a negative test with a non-zip renamed to `.zip`.

## C. Wi-Fi connect diagnostics ("long passwords don't work")

KNOWN-ISSUES.md records that a long Wi-Fi password could not be entered
from the F8 menu. Code reading shows the UI edit field scrolls fine up to
79 characters and the socket path carries 1 KB, so the likely culprits are
wpa_supplicant rejecting the passphrase (WPA2 passphrases are 8–63
characters; 64 hex digits is a raw PSK and anything else is `FAIL`), a
wrong key, or characters the Atari keyboard has no key for (`{ } ~ \``:
not fixable here, document it). Today the menu says "Connecting..." then
reports the raw `wpa_state`, which hides all of that. Make it visible:

- `init/init.c` `cmd_wifi_connect()`: log (never the password itself)
  `wifi: ssid "<ssid>" psk_len <n>` and the wpa_cli reply of the
  `set_network … psk` call; if that reply is not `OK`, remove the network
  and reply `ERR password rejected by wpa_supplicant (8-63 characters)`.
- Start `wpa_supplicant` with `-f /run/wpa.log` (see the argv around
  `init.c:1115`) and add to the `status` reply a line
  `wifierr <text>` derived from the last relevant line of that log:
  `CTRL-EVENT-SSID-TEMP-DISABLED … reason=WRONG_KEY` → `wrong password`;
  `reason=CONN_FAILED` / `AUTH_FAILED` → `connection failed`;
  `CTRL-EVENT-NETWORK-NOT-FOUND` → `network not found`. Only when the
  current wpa_state is not COMPLETED; otherwise omit the line.
- `emulator/kms/sysmenu.c` connect flow: instead of `wait_seconds(8.0)`
  then one status read, poll `status` every second for up to 20 s,
  updating a `fMessage("Connecting... <state>", FALSE)` line; stop early
  on `wifi connected` (then also wait for `ip`) or on a `wifierr` line.
  Final message: `Connected: <ssid> (<ip>)` or `Wi-Fi: <wifierr text>`
  (fallback: the state, as today). On `ERR …` from the connect command,
  show the ERR text.
- The System menu password prompt gets the title
  `Password (8-63 chars; no { } ~ \`)` so the limit is visible.
- KNOWN-ISSUES.md: replace the long-password entry with what is now known
  (limits above, where to look: `/atari/logs`, the `wifierr` line) and keep
  the `wpa_supplicant.conf` workaround.

Verification: builds clean (init is static, emulator host build
warning-free); in QEMU there is no Wi-Fi adapter, so check that the F8
Wi-Fi menu still says there is none and nothing else regressed (menu
screenshot). The behaviour with a wrong/long password is verified by Claude
on the laptop.
