# Spec: polish round 2 — smooth scrolling, power/thermals, FujiNet SD

Status: from Carlos's laptop session 2026-11-07 ("scrolling is not as
smooth as it could be", "mild fan activity", "since there is no sd, there
is no way to copy things"). Design: Claude (Fable); implementer: an Opus
agent; measurements and laptop verification: Claude. Rules as in
`docs/specs/library.md` §3/§7 and `docs/specs/polish-1.md`.

## A. Smooth scrolling

### Diagnosis (code reading; to be confirmed on the ZenBook)

The emulator paces itself with `Atari800_Sync()` (nanosleep to 50 Hz PAL /
59.92 Hz NTSC) and presents with a DRM page flip that lands on the panel's
next vblank (`KMS_VIDEO_DisplayScreen`, `wait_flip`). A 50 Hz Atari on a
60 Hz panel therefore shows every fifth frame twice: inherent 5:6 judder on
any smooth scroller, regardless of how good the flips are. On top of that,
sleeping to a software clock that is not the panel's clock makes the
cadence drift, so the doubled frame wanders instead of repeating regularly
(that looks worse than a fixed 5:6 pattern). The fix is to let the display
drive the emulation whenever the two rates are close, and to give the user
a 60 Hz option.

### Wanted

1. **Mode choice honours the TV system.** In `pick_output()` (video.c),
   among the connector's modes at the preferred mode's resolution, prefer
   the one whose refresh (vrefresh, or clock/(htotal*vtotal)) is closest to
   the emulated rate: 50 Hz for PAL (`Atari800_tv_mode == Atari800_TV_PAL`),
   60 Hz for NTSC; ties → the preferred mode. Log the candidate list once
   (`kms: modes: 1920x1080@60 1920x1080@50* …`). `-kms-mode WxH@R`
   overrides (config `KMS_MODE`). Laptop panels rarely list 50 Hz, but
   external monitors and some panels do, and then PAL is perfect.
2. **Vblank-locked pacing.** When `|panel refresh − emulated fps| < 0.75`
   Hz, pacing switches to the flip: define `SUPPORTS_PLATFORM_SLEEP` for
   the KMS build (configure.ac, in the existing patch) and implement
   `PLATFORM_Sleep(double s)` in `emulator/kms/video.c`: in locked mode it
   returns immediately (the `wait_flip()` at the start of
   `KMS_VIDEO_DisplayScreen` is the clock); otherwise it nanosleeps like
   `Util_sleep`. atari800's `Sound_AdjustSpeed()` already nudges the frame
   time to keep the audio buffer level, so audio stays in sync with the
   slightly different clock (59.92 vs 60.00). Log `kms: pacing locked to
   the display (60.00 Hz)` / `kms: pacing by timer (panel 60 Hz, Atari 49.86 Hz)`.
   Also when locked, never drop a frame on `EBUSY`: wait for the pending
   flip first (that is what `wait_flip` does; make sure the order
   wait→render→flip holds and that a turbo (F12) run falls back to timer
   pacing, i.e. the lock applies only when `!Atari800_turbo`).
3. **TV system in the System menu.** New item `TV system: PAL / NTSC`
   (popup). Switching calls `Atari800_SetTVMode()` (+ re-pick the mode if
   a matching refresh exists → requires re-modeset; simplest: switch TV
   mode live, re-evaluate the pacing lock, and only change the display
   mode on the next emulator start; say so in the item's message:
   `NTSC: 60 Hz, smooth on this screen` / `PAL: 50 Hz (this screen is 60
   Hz: slight judder)`), persists via atari800's `DEFAULT_TV_MODE`.
   **Default: NTSC** (decided 2026-11-07: "Atari was an NTSC first
   machine"; a 60 Hz panel is the common case). The config seed in init
   (and any seed in mkimage.sh) writes `DEFAULT_TV_MODE=NTSC`; an existing
   `atari800.cfg` that says PAL is the user's choice and is left alone. The hardware page shows
   `Display: 1920x1080 @ 60 Hz, Atari PAL 49.9 Hz, pacing: timer|display`.
4. **Stats for me**: `-kms-debug` already logs fps/flips/timeouts/busy
   every 5 s; add the number of frames the flip handler reports as
   *repeated* (vblank sequence gaps > 1 between flips: use the `frame`
   argument of the page-flip event) so judder is measurable from the log:
   `kms: 50.0 fps, flips 250, repeats 50, …`.

### Verification

QEMU's virtual display has no real vblank timing (flips complete
immediately) so only the logic can be checked there: with `-kms-debug`,
`DEFAULT_TV_MODE=NTSC` must log "pacing locked" when the reported mode is
60 Hz (bochs reports 60), PAL must log "pacing by timer". Both must run
with sound and a working F8 menu (screenshots of the new item and of the
hardware page). The real check is mine on the laptop (scroller demo, before
/after `repeats` counts, PAL vs NTSC).

## B. Power and thermals (fan)

### Measurements first (Claude, on the laptop; the agent adds the tooling)

Add to init: `atari.perf=1` on the kernel command line (and the socket
command `perf`) logs every 10 s to `/atari/logs/perf.log` (and `logmsg`
in debug): per-process CPU% for atari800, fujinet, wpa_supplicant,
dropbear, init (from `/proc/<pid>/stat` deltas), package power in W from
`/sys/class/powercap/intel-rapl:0/energy_uj` deltas (when present), CPU
temperature from the first `hwmon` with a `temp1_input` named `coretemp`
or `k10temp`, and the current cpufreq MHz of cpu0. The **hardware page**
shows one line `CPU: 12% emu, 3% fujinet · 48 °C · 2.1 W` built from the
same source via `status` (add `perf <emu%> <fuji%> <tempC> <watts>` to
the status reply; init samples it in its main loop at most once a second
when asked).

### Known-good laptop power defaults init should set (cheap, reversible)

At boot, after coldplug, ignoring failures and logging what was applied:

- `/sys/module/pcie_aspm/parameters/policy` → `powersave`
- every `/sys/class/scsi_host/host*/link_power_management_policy` →
  `med_power_with_dipm`
- every `/sys/bus/usb/devices/*/power/control` → `auto` (not for the
  device the boot stick hangs from: find it via `/sys/block/<stick>/device`
  ancestry, and skip HID devices too: keyboards stutter on resume)
- every `/sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference`
  → `balance_power` (only if the file exists: intel_pstate active mode)
- `/sys/class/drm/card*/device/power/control` → `auto`
- kernel cmdline additions in `mkimage.sh`: `i915.enable_psr=1
  i915.enable_fbc=1` (Intel panel self refresh and framebuffer
  compression; both default on in recent kernels but make it explicit),
  `nmi_watchdog=0`.
- Wi-Fi: `iwlmvm power_scheme=1` is already set (that is *performance*;
  keep it, latency mattered more).

### Process-level fixes

- **FujiNet-PC** idles in a 1 ms `select` loop (`_netsio.poll(1)` in
  `lib/bus/sio/sio.cpp`) and services the web server and task manager on
  every iteration: ~1000 wake-ups/s. In the PC-only path, back off to
  `poll(5)` after 200 ms without bus activity and return to `poll(1)` on
  the first byte (patch `fujinet/patches/0003-pc-idle-backoff.patch`).
  Expect a few percent of a core saved; the real number comes from the
  perf log.
- **Emulator**: no change until measured. If the perf log shows atari800
  above ~20% of a core at BASIC READY on the i7, look at `render()` first
  (it rewrites the whole 1344x960 area every frame even when the Atari
  screen did not change: a per-row `memcmp` against the previous source
  rows would skip unchanged rows cheaply; only do this if measured).

### Verification

Builds; in QEMU `atari.perf=1` produces the log with sane numbers (CPU%
non-zero, power/temperature "n/a"), the hardware page shows the line,
sysfs writes are logged (most files won't exist in QEMU; that must not
error). Laptop numbers are mine.

## C. FujiNet SD: "no way to copy things"

Verified: FujiNet-PC mounts its SD from the folder init passes (`-s
/media/atari/fujinet/SD`; test run in the build env printed `SD mounted`),
and the shipped `fnconfig.ini` has `Host1 = SD`. So the SD exists; what is
missing is discoverability and a path to put files on it:

1. Seed `/atari/fujinet/SD/README.TXT` (ATASCII) in `mkimage.sh`: "This is
   FujiNet's SD card. Put ATR/XEX here from the Library (F9 → Copy to
   FujiNet SD) or over SSH/USB; CONFIG lists it as host SD."
2. README "FujiNet" section: how files get to the SD (Library copy, USB
   copy, CONFIG's own copy between hosts, and that TNFS hosts are
   read-only), and that D1–D8 mounts made in CONFIG are FujiNet's, not the
   Library's.
3. In the Library, the FujiNet SD tab title becomes `FujiNet SD (host SD
   in CONFIG)`.

The remaining question (what Carlos actually saw: an empty SD listing, a
CONFIG error on the SD host, or something else) is checked by Claude on the
laptop (`/atari/logs`, FujiNet's log for `SD mounted`, CONFIG's SD host).
