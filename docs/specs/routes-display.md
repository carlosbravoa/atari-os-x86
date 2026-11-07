# Spec: every route boots, the Library sees the whole stick, display options

Status: from Carlos's session 2026-11-07 ("we need all routes to work",
"boot to self test or no basic", "scanlines in different opacity, wide
screen fill"). Design: Claude (Fable); implementer: an Opus agent; QEMU and
laptop verification: Claude. Rules as in `docs/specs/library.md` §3/§7.

## A. The Library browses the whole stick

Today the Library tab is rooted at `/atari/library`, so FujiNet's downloads
(`/atari/fujinet/SD`), `/atari/disks`, `/atari/hdd` and `/atari/ash` are
out of reach unless a dedicated tab exists.

- The first tab becomes **Stick**, rooted at `/media/atari` (MEDIA_USER),
  opening in `library/` on first use; `..`/Backspace goes up to the root,
  Esc at the root closes. The title shows `Stick: /library`,
  `Stick: /fujinet/SD`. Recent/Favourites blocks stay on this tab's root
  view (`Stick: /`) **and** in `library/` as today.
- Keep the USB tabs. Drop the separate FujiNet SD tab (it is now
  `Stick: /fujinet/SD`); the popup's `Copy to FujiNet SD` and `Copy to
  Library` stay, plus a new `Copy to…` only when a USB drive is present.
- Hide `config/`, `ssh/` and `logs/` in the Stick tab (nothing runnable,
  and `ssh/` holds keys). `ash/` is shown: Run on `ASH.CAR` boots ASH.
- `library.cfg` entries keep absolute paths; nothing to migrate.

## B. Every route boots what the user asked for

### The problem

Mounting an ATR from atari800's F1 menu and cold-starting boots ASH (the
cartridge wins) or FujiNet CONFIG (FujiNet serves D1:), and when BASIC is
on, most game disks fail. The Library's Run works only because it takes
the cartridge out itself. Rule wanted: **what is in D1: boots**, and the
configured boot target applies only when nothing is mounted.

### Boot targets

`boot=basic|nobasic|ash|fujinet` (add `nobasic`: BASIC disabled, nothing
mounted → the XL self test, exactly like holding OPTION on an empty
machine). F8 popup `Boot into: BASIC / No BASIC (self test) / ASH /
FujiNet CONFIG`. init adds nothing for `nobasic` (the emulator handles it);
`status` reports it like the others; ASH's STATUS listing shows
`boot: nobasic`.

### The cold-start rule (emulator side)

A hook `KMS_BOOT_ColdStart()` next to `KMS_HDEV_ColdStart()` in
`Atari800_Coldstart()` (extend the `#ifdef KMS` block in `src/atari.c`),
implemented in a new `emulator/kms/boot.c`/`kms_boot.h`, applies before
the machine restarts:

1. **Something to boot is mounted** — a local disk in D1
   (`SIO_drive_status[0]` is `SIO_READ_ONLY`/`SIO_READ_WRITE`), a
   cassette with "boot from tape" set, or a cartridge that is not our
   `ASH.CAR` — then: if the inserted cartridge is `ASH.CAR` (compare the
   basename of `CARTRIDGE_main.filename`), remove it for this boot and
   remember to put it back; disable BASIC for this boot
   (`Atari800_disable_basic = TRUE`, unless the one-shot flag
   `KMS_BOOT_with_basic` was set by "Run with BASIC", §B.4).
2. **Nothing mounted** — apply the boot target: `basic` → BASIC on, no
   cartridge (if the ASH cart was removed earlier, keep it out); `nobasic`
   → BASIC off; `ash` → insert `/atari/ash/ASH.CAR` if not inserted,
   BASIC off; `fujinet` → BASIC off (CONFIG does not need it and the games
   FujiNet mounts do need it off; verify CONFIG still boots).
3. Log one line: `boot: cold start -> disk D1 (BASIC off, ASH cart out)` /
   `boot: cold start -> target ash`.

This hook makes F1 "Cold start", Shift+F5, the Library's Run, ASH's own
`mount`+`reboot`, and the sysmenu all behave the same way. The Library's
Run keeps calling `AFILE_OpenFile(path, TRUE, …)`; the explicit
`CARTRIDGE_Remove()` it does today can stay.

### Eject and return

- F8: `Eject all, back to <target>` → `SIO_Dismount` D1–D8, remove any
  cartridge that is not `ASH.CAR`, clear tape, then `Atari800_Coldstart()`
  (rule 2 applies). Also offered in the Library's popup when something is
  mounted: `Eject all & reboot`.
- `Mount as D1:` in the Library (no reboot) stays; the next cold start
  boots it by rule 1.

### Run with BASIC

The Library popup gets `Run with BASIC` under `Run` for `atr/xfd/dcm/atx`
(BASIC-based disks such as magazine disks): sets `KMS_BOOT_with_basic`
for the next cold start, then runs as `Run` does. `bas`/`lst` keep
atari800's behaviour (BASIC on, program entered).

### FujiNet mode

With `boot=fujinet` atari800 runs with `-netsio` and SIO patch off. Find
out (read `pokey.c` around `POKEY_siocheck`/`SIO_PutByte`, `sio.c`'s
serial-level disk handling and `netsio.c`) whether a locally mounted D1:
answers before the frame is forwarded to FujiNet-PC. Required behaviour:
a local D1: image boots and FujiNet gets the drive only when no local image
is mounted. If atari800 already does this, verify and document; if not,
implement the smallest change in the serial path (guarded by `#ifdef KMS`
in the patch) and say so. The reverse (FujiNet's mounted D1: while a local
image is also mounted) is the user's choice: local wins.

## C. Display options (F8 → `Display…`)

A submenu, everything live (the menu itself is drawn through the same
renderer, so the change is visible immediately), persisted in
`atari800.cfg` as `KMS_*` keys via `KMS_VIDEO_ReadConfig/WriteConfig`:

1. **Scaling**: `Integer` (today's default: largest integer factor,
   centred), `Fit` (largest size keeping the 4:3-ish aspect, non-integer,
   centred), `Fill` (stretch to the whole panel, wide). The existing
   `KMS_SCALE` modes INTEGER/FIT/STRETCH map 1:1; just expose them.
2. **Smoothing** (Fit/Fill only): `Sharp` (nearest, today) / `Smooth`
   (bilinear on the horizontal axis at least; full bilinear if the cost at
   1080p stays under ~3 ms per frame on the i7, measure with `-kms-debug`
   and report). Ignored for Integer.
3. **Scanlines**: `Off` / `Light` / `Medium` / `Strong` = darken every
   second output line of each Atari line to 75 / 50 / 25 % brightness.
   Only when the vertical factor is ≥ 2 (integer or fit); the menu says
   `(needs 2x)` otherwise. Implement in the row loop (`render()`), as a
   second palette (`palette_dim[]`) for the dark rows so the fast path
   stays a palette lookup plus memcpy.
4. **Borders**: `Full` (384 px: everything the ANTIC generates), `TV`
   (336 px, today), `Tight` (320 px). Sets `Screen_visible_x1/x2` and
   recomputes the layout.
5. **TV look** (NTSC only): `Crisp` / `NTSC TV` using atari800's built-in
   Blargg NTSC filter: build with `--enable-ntscfilter` in
   `scripts/build-emulator.sh`, feed `Screen_atari` through
   `FILTER_NTSC_Process()` into a 32-bit intermediate (`FILTER_NTSC_width`
   = 2x output width) and scale that with the same scaler (a 32-bit source
   path next to the 8-bit one). Greyed out `(PAL)` when the TV system is
   PAL. Default `Crisp`. Measure CPU with `-kms-debug` and the perf line;
   if the filter makes the i7 miss 60 fps, say so and keep it as an option
   anyway.
6. The hardware page's `Display:` line gains the scaling mode and
   scanlines.

Defaults unchanged (Integer, Sharp, Off, TV, Crisp): a fresh stick looks
as today.

## D. Verification (QEMU, `scripts/qemu-test.sh`)

Screenshots under `build/qemu-test/`, log lines from `/run/atari800.log`:

- A: Stick tab at `/library`, `..` to `Stick: /`, into `fujinet/SD`
  (seed a test ATR there via `mk-usb-test.sh`-style seeding in the test
  image copy, or copy one from the USB test drive through the Library
  first), Run it; `config/`/`ssh/` absent from the listing.
- B: boot targets: `nobasic` shows the self test (screenshot); with
  `boot=ash`, F1 → Disk → mount `A.ATR` → cold start boots the disk (not
  ASH), log line; `Eject all, back to ASH` returns to ASH; same with
  `boot=basic` and `boot=fujinet` (CONFIG returns after eject); `Run with
  BASIC` on an ATR keeps BASIC (check with `peek`/the BASIC READY prompt
  after booting a plain DOS disk: the test DOS ATR from FujiNet's dist,
  `autorun.atr`, boots to its menu either way, so use the hardware page's
  or a log line `BASIC on/off` to prove the state).
- C: one screenshot per option value at 1280x800 (Integer/Fit/Fill;
  scanlines Light/Medium/Strong; Borders Full/Tight; NTSC TV look), the
  `Display…` menu itself, `-kms-debug` fps lines showing 60 fps is still
  held with NTSC TV + Fit + Smooth on this machine (QEMU, KVM: note the
  CPU% from `perf` too).
- Host `-Werror` build clean; `mkimage` "Done".
