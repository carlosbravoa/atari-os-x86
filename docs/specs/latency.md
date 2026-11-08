# Spec: input lag, controller to display

Status: Carlos's session 2026-11-08 ("improving input lag. From controller
to display. That is worth a full revision"). Phases 1 and 2 are this
spec's deliverable; 3 and 4 are outlined for the next one. Design: Claude
(Fable); implementer: an Opus agent; verification: Claude (QEMU numbers
are indicative only: no real panel) and Carlos on the ZenBook (240 fps
phone video of key and screen for one ground truth). Rules as in
`docs/specs/menus.md`.

## Today's pipeline (what the numbers come from)

- `kms/input.c` reads evdev once per frame from `PLATFORM_Keyboard()`
  at the top of the main loop (`kms/main.c`), then `Atari800_Frame()`
  emulates a whole frame, `PLATFORM_DisplayScreen()` blits and queues a
  page flip (`DRM_MODE_PAGE_FLIP_EVENT`, `flip_pending`), and
  `PLATFORM_Sleep()` waits for the flip event when pacing is locked
  (`KMS_VIDEO_PacingLocked()`), else nanosleeps.
- So the input sampled at the start of frame N is shown when frame N's
  flip lands: emulate time + wait for the vblank + one scanout + panel.
- USB HID devices poll at their descriptor interval (pads 4-8 ms,
  keyboards 8 ms). Kernel command line (`scripts/mkimage.sh` CMDLINE) has
  `i915.enable_psr=1` (Panel Self Refresh, adds exit latency on every
  update). init sets `energy_performance_preference=balance_power` and
  USB autosuspend (HID devices kept on) for quiet fans.
- ALSA: period = rate/50 (20 ms), buffer = 4 periods = 80 ms
  (`sound_alsa.c` ~127).

## Phase 1: measure

Add to `kms/input.c` / `kms/video.c` / `kms/main.c`:

- Every evdev key/button/axis event carries the kernel's timestamp
  (`struct input_event` time). Record, per frame, the oldest event
  timestamp consumed in this frame's `PLATFORM_Keyboard()` (or none).
- The DRM page flip event carries the vblank timestamp (sec/usec in
  `page_flip_handler`). When the flip for a frame that consumed input
  completes, latency sample = flip timestamp - event timestamp. Keep a
  ring of the last 120 samples; also note per frame: emulate+blit time
  (`Util_time()` around `Atari800_Frame()` + blit), time waited for the
  flip.
- Clocks: evdev uses CLOCK_MONOTONIC when the fd is set with
  `EVIOCSCLOCKID`; set it (default is REALTIME). DRM flip timestamps are
  monotonic when `DRM_CAP_TIMESTAMP_MONOTONIC` is on (it is by default
  on modern kernels; query it, otherwise fall back to realtime for both).
- Expose: `KMS_VIDEO_Latency(&avg_ms, &max_ms, &n)` and the frame times.
  Hardware page line: `Input lag: 31 ms avg, 44 max (120)` (shown as
  `Input lag: no input yet` before any sample), and `Frame: 1.9 ms emu,
  13 ms wait`. `-kms-debug` logs the stats every 5 s; the perf log init
  keeps (`atari/logs/perf.log`, via the ctl `perf` line) gets the avg
  appended if that is a one-line change in init, else skip.
- Nothing changes in behaviour in this phase. Baseline numbers are
  recorded in the commit message (QEMU) and by Carlos on the ZenBook.

## Phase 2: cheap wins (each one switchable, measured on/off)

1. **USB 1 ms polling**: CMDLINE gets `usbhid.jspoll=1 usbhid.kbpoll=1
   usbhid.mousepoll=1`. Document in README (Hardware).
2. **PSR off**: `i915.enable_psr=0` (keep `enable_fbc=1`). README notes
   the small battery cost. (We ship no battery-saving promise; the fan
   work was about CPU heat.)
3. **Low-latency power mode**, F8 > Controls `Low latency: on|off`
   (saved, `KMS_LOW_LATENCY=1`, default **on**): the emulator asks init
   (new ctl command `power fast|quiet`) to set
   `energy_performance_preference=performance` (or governor
   `performance` on CPUs without EPP) while on; `balance_power` when
   off. init keeps the setting for the session; at boot it applies the
   emulator's saved choice once the config is read (the emulator sends it
   at start). The Hardware page's "CPU: ..." line adds `, fast` / `,
   quiet`.
4. **Timer precision**: the emulator sets `prctl(PR_SET_TIMERSLACK, 50
   us)` and, when running as root (we do), `SCHED_FIFO` priority 10 for
   the main thread with `mlockall(MCL_CURRENT|MCL_FUTURE)`; if either
   fails, log and continue. FujiNet-PC and the ALSA thread stay as they
   are.
5. **Audio buffer**: ALSA period rate/100 (10 ms) and buffer 3 periods
   (30 ms) when low latency is on; today's 20/80 when off. Underruns are
   counted and logged (`-kms-debug`); if the stick's QEMU run shows
   underruns at 30 ms, use 40 ms and say so.
6. **Input read placement**: move `PLATFORM_Keyboard()` so it runs after
   `PLATFORM_Sleep()` returns (i.e. right after the previous flip lands,
   just before `Atari800_Frame()`), not before the menu checks at the
   top of the loop; the menu requests are polled from the same read.
   This alone removes the "wait for flip with stale input" case when the
   frame is fast.

Expected after phase 2 (ZenBook, 60 Hz): roughly 20 ms less than the
baseline. Record before/after from the Hardware page.

## Phase 3 (next spec): just-in-time frame

Sleep until (next vblank - emulate_blit_time_p95 - 1.5 ms margin), then
read input, emulate, blit, flip. Needs the flip timestamps of phase 1
and a per-mode render-time estimate; falls back to today's loop when the
frame takes longer than a period twice in a row.

## Phase 4 (next spec): run-ahead

One frame of run-ahead with atari800's savestates (`StateSav`): emulate
frame N, save, emulate N+1 with the same input, display N+1, restore.
Option in Controls, off by default; doubles CPU time per frame.

## Docs

README: a "Input lag" subsection under Hardware or Display (what is
measured, the Low latency switch, kernel options), CHANGELOG, KNOWN-ISSUES
(numbers are without the panel's own response time).

## Verification

QEMU: the Hardware page shows numbers after keys are typed; the stats
move when Low latency is toggled (sound buffer line in the log changes;
`cat /sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference`
if the QEMU CPU exposes it, else skip); `cat /proc/cmdline` has the new
options; `/sys/module/usbhid/parameters/jspoll` = 1; emulator log shows
`sched: fifo 10, timerslack 50us, mlockall ok`. The ZenBook: before/after
numbers from the Hardware page, Wi-Fi and sound still fine, fan noise
acceptable to Carlos with Low latency on (else default off).
