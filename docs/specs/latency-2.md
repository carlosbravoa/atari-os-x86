# Spec: input lag, phase 3: the just-in-time frame

Status: follows `docs/specs/latency.md` (phases 1 and 2, shipped in
v0.1.0). Carlos's ZenBook numbers from the Hardware page on 2026-11-08
(1920x1080 at 60.01 Hz, Free 5.00 x 4.00, Scanlines Strong, pacing
display) are the baseline. Design: Claude (Fable); implementer: an Opus
agent; verification: Claude in QEMU and on the ZenBook, Carlos with the
pad. Rules as in `docs/specs/menus.md` §0 and `docs/specs/library.md`
§3/§7 (patch regeneration, `-Werror`, C90, 38-column lines, nothing
committed by the agent). Phase 4 (run-ahead) stays a separate spec.

## The baseline, and what it says

| Mode | Input lag avg / max | Frame emu / wait | Wake p50 / p99 |
|---|---|---|---|
| Low latency on (fast) | 26 / 34 ms | 0.8 / 16 ms | 0.12 / 0.18 ms |
| Normal (quiet) | 41 / 49 ms | 1.9 / 15 ms | 0.20 / 0.24 ms |

One display period P is 16.67 ms. Input lag is measured from the evdev
event stamp to the vblank stamp of the flip that shows the frame
emulated from it (`kms/video.c`, `flip_input_ts`).

**Low latency = 1.5 frames, the floor of today's loop.** `KMS_VIDEO_WaitFrame()`
returns right after vblank N; the input is read, the frame emulated and
rendered in 0.8 ms, the flip queued and lands at vblank N+1. An event
that arrived t before vblank N (t uniform in 0..P) waits t + P: the mean
is P/2 + P = 25 ms, the max 2P = 33 ms. Measured 26 / 34.

**Normal = 2.5 frames: a phase accident, not CPU.** Without `WaitFrame`
the input is read wherever atari800's own frame sleep ends. That sleep
(`PLATFORM_Sleep` → `flip_wait_for(s)`) has a budget of one Atari frame
(16.69 ms) minus the emulation time already spent, so it ends 14.8 ms
after the previous flip was queued, 0.1 ms or so *before* the flip's
vblank. The loop then reads the input just before vblank N, emulates,
and `KMS_VIDEO_DisplayScreen()`'s `wait_flip()` holds the finished frame
until vblank N arrives before it can even queue the flip, which lands at
N+1: the input read just before a vblank is shown two vblanks later.
Mean P/2 + 2P = 41.7 ms, max 3P = 50. Measured 41 / 49.

So:

1. Reading the input after the vblank wait is a free fix for every mode:
   same CPU, same sleeps, one frame less. It becomes the default.
2. Low latency gains a real "just in time" frame: sleep until shortly
   before the next vblank, then read, emulate, render, flip. Mean lag
   P/2 + lead, with lead ~3 ms: about 11 ms avg, 20 max. That is the
   last frame that the PC side can remove; what remains is the game's
   own VBI-to-draw latency (phase 4's territory) and the panel.

## A. Vblank-aligned read in every mode (default on)

- `kms/main.c`: call `KMS_VIDEO_WaitFrame()` unconditionally before
  `PLATFORM_Keyboard()`, not only when `KMS_SYSMENU_low_latency`. It is
  a no-op when pacing is not locked (timer pacing, Turbo) or no flip is
  pending, and `KMS_VIDEO_TakeFramePaced()` already makes atari800's
  frame sleep return at once after it, so the loop runs exactly one
  Atari frame per vblank as it does today in Low latency.
- Nothing else of Low latency moves: EPP fast, SCHED_FIFO, timerslack,
  mlockall and the 40 ms ALSA buffer remain behind the switch (Carlos:
  the aggressive settings are an opt-in). `KMS_SYSMENU_low_latency`
  itself stays as it is.
- Check in `-kms-debug` that the Normal-mode `Frame:` wait drops from
  ~15 to ~16 ms of pure vblank wait with emu unchanged, and that
  `st_repeats`/`st_timeouts` do not grow (the Atari 59.92 Hz vs display
  60.01 Hz drift is handled as today by `pacing_check()`).

## B. Just-in-time frame (Low latency only)

### Timing

- After `WaitFrame()` has returned from the flip that landed at vblank
  N (stamp `last_flip_ts`, period P from the mode's refresh as
  `KMS_VIDEO_Refresh()`/the value the Hardware page shows; use the
  measured mean of the last 60 flip-to-flip intervals when it is within
  2% of the mode's, else the mode's), sleep until
  `target = last_flip_ts + P - lead` with `clock_nanosleep(CLOCK_MONOTONIC,
  TIMER_ABSTIME, ...)` on the clock the flip stamps use (`lat_clock`; if
  it is `CLOCK_REALTIME` on some machine, convert the target). Feed the
  wake lateness into `wake_sample()` like `PLATFORM_Sleep()` does and
  count it as wait time (`frame_waited`) so the `Frame:` line stays
  meaningful. Then return: the main loop reads the input, emulates and
  renders, and the flip lands at vblank N+1.
- `lead = max(p95(fr_emu over the last 120 valid frames) + 1.0 ms,
  2.0 ms) + adapt`, where `adapt` starts at 0, grows by 1.0 ms on every
  late frame and shrinks by 0.5 ms (never below 0) after 600 consecutive
  frames that were on time. `lead` is capped at P - 1 ms, where the
  loop is today's Low latency behaviour (read right after the vblank),
  so the worst case of adapting is the current behaviour, never worse.
  `fr_emu` already is emulate + render time (total minus waits), which
  is exactly what has to fit between the read and the vblank.
- **Late frame** = the flip queued for this frame landed at vblank N+2
  or later: in `page_flip_handler()` the vblank sequence `frame` is 2 or
  more past the previous flip's sequence while JIT was armed for that
  frame (the repeat detection for `st_repeats` already sees this gap;
  tell the two apart: a planned repeat from `pacing_check()` is not a
  late frame). A late frame costs one duplicated picture; the adaptive
  lead makes it rare.
- **Give up** when emulate + render exceeded P on two consecutive frames
  (the machine is too slow for the mode, e.g. a heavy TV look preset on
  a weak laptop): disable JIT until the display settings change
  (`KMS_VIDEO_Set*`/mode change) or Low latency is toggled, and log
  once `kms: jit off, frame %.1f ms > period`. Today's loop continues.
- JIT is armed only when `KMS_SYSMENU_low_latency`, pacing is locked,
  `flip_supported`, and the previous flip has a stamp. Menus (`UI_is_active`),
  Turbo and timer pacing go through today's path untouched.
- Config: `KMS_JIT=0|1` (default 1) in the emulator config, read with
  the other `KMS_*` keys, not in any menu: a debugging switch so the
  effect can be measured on/off with Low latency on. Document it in the
  README's "Input lag" paragraph in one sentence.

### Sound

The ALSA write moves from right after the vblank to ~3 ms before the
next one; with Low latency's 40 ms buffer (4 x 10 ms periods) this is
within budget. Watch the underrun counter in the `-kms-debug` log: it
must stay at 0 over a minute in QEMU with JIT on. If it does not, the
Low latency buffer goes to 50 ms (5 periods) and the README number is
updated; do not touch the Normal-mode buffer.

### Hardware page and log

- `Frame:` line (38 columns): `Frame: 0.8 ms emu, lead 3.0, late 0`
  when JIT is active (`late` = late frames in the 120-frame ring, so the
  user sees stutter risk), today's `Frame: 0.8 ms emu, 16 ms wait`
  otherwise. Keep `hardware_lines()` the single source so Report a
  problem gets the same text.
- `-kms-debug` 5-second line gains `jit lead %.1f ms, late %lu` (or
  `jit off`) next to `pacing`.

## Docs

- README "Input lag": the table of what to expect at 60 Hz (Normal about
  1.5 frames, Low latency about P/2 + 3 ms), one sentence on `KMS_JIT`,
  and that a `late` count above a handful per minute means the machine
  cannot keep up and Low latency should go off or the TV look to Crisp.
- CHANGELOG under Unreleased: the Normal-mode fix (one frame less for
  everyone) and the just-in-time frame in Low latency.
- KNOWN-ISSUES: the numbers exclude the panel's own response and the
  game's internal input-to-draw delay; a late frame shows as one
  repeated picture.

## Verification (agent, in QEMU; pacing locks to the display there)

1. Host build `-Werror` clean; image built; debug copy per the rules.
2. Normal mode: type keys for 20 s on the BASIC prompt, open Hardware
   (F8, down 8, RETURN): `Input lag` avg must be about P/2 + P (24-27 ms
   at 60 Hz; before this change QEMU showed the 2.5-frame value) and
   `Frame:` wait about P minus emu.
3. Low latency on (F8 > Controls, Low latency), same test: `Input lag`
   avg about P/2 + lead (10-13 ms), max under 1.3 P, `late 0` or close;
   `-kms-debug` log shows `jit lead`, `repeats` unchanged, ALSA
   underruns 0 over a minute.
4. `KMS_JIT=0` in the config file (`serial` to append it, restart the
   emulator through the ctl socket): Low latency numbers return to the
   1.5-frame value. Remove it afterwards.
5. Turbo (F12) and the menus still work; Scaling/TV look changes do not
   crash the JIT path (they go through the mode-change reset).
6. Screenshots of the Hardware page for 2 and 3 in `build/qemu-test/`,
   names given in the report; delete any other large file created in
   `build/`.

On the ZenBook (Claude after deploy, Carlos later): the Normal-mode
`Input lag` should read ~25 ms, Low latency ~11 ms, with `late` staying
at 0 while playing; sound without clicks in both modes.
