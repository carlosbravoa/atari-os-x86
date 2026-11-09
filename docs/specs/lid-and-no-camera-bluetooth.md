# Spec: the lid switch, and no camera or Bluetooth drivers

Status: Carlos, 2026-11-09: "lid work and removing support for camera and
bluetooth". Design: Claude (Fable); implementer: an Opus agent in the main
tree; verification: Claude in QEMU and on the ZenBook with Carlos closing
the lid. Rules as in `docs/specs/menus.md` §0 and `docs/specs/library.md`
§3/§7 (`-Werror`, C90 for the emulator, nothing committed by the agent,
never touch `flash-it.sh`, keep `build/` small: delete every debug image
and test file you create, except as said under Verification).

## A. Closing the lid: screen off and pause; opening it: back

Today nothing happens: the panel stays lit and the Atari runs with the
lid closed. Real suspend is deferred (`project-architecture-decisions`:
suspend/resume is a later, opt-in experiment); this is the cheap and safe
step: the picture goes off, the emulation pauses, the machine keeps
running (Wi-Fi, SSH, FujiNet stay up).

The ZenBook shows the parts are there: `/sys/class/input/*/name` has
`Lid Switch` (the kernel `button` driver, an evdev device with `EV_SW` /
`SW_LID`, no `EV_KEY`), `/proc/acpi/button/lid/LID0/state` says open or
closed. The emulator's input code opens only devices with `EV_KEY`
(`emulator/kms/input.c` around line 132), so the lid device is not read
yet.

1. **`emulator/kms/input.c`**: also open evdev devices that have `EV_SW`
   with `SW_LID` (keep the `EV_KEY` rule for everything else; a device
   may have both). Read the initial state with `EVIOCGSW` when opening
   (a machine booted with the lid closed, or the emulator restarted).
   Track it in `int KMS_INPUT_lid_closed` (header `kms_input.h`), set
   from `EV_SW`/`SW_LID` events in the normal event path; hotplug and
   device removal as for the other devices (a removed lid device means
   open). Add `int KMS_INPUT_WaitEvent(double seconds)`: `poll()` on all
   the evdev fds (and the hotplug source the input code already has) for
   up to `seconds`, then process whatever arrived exactly as the per-frame
   read does, returning whether anything came in. If a function like it
   exists for the menus, reuse or extend it instead.
2. **`emulator/kms/video.c`**: `void KMS_VIDEO_Blank(int off)`. Only when
   the active connector is internal (`DRM_MODE_CONNECTOR_eDP`, `LVDS`,
   `DSI`; the type is known where the connector is chosen, near line
   411): `off` sets the connector's `DPMS` property to
   `DRM_MODE_DPMS_OFF` (`drmModeConnectorSetProperty`; find the property
   id once from `drmModeObjectGetProperties` on the connector; when there
   is no such property, say so in the log once and only pause), on
   `!off` set `DRM_MODE_DPMS_ON` and then `drmModeSetCrtc` with the
   current mode and the front buffer so the flip chain restarts cleanly,
   and reset the pacing and JIT state (`flip_pending`, `have_seq`,
   `jit_reset()`, the input-lag and wake rings: a stale first flip after
   the wake must not count). With an external connector active (a
   clamshell setup: lid closed, HDMI monitor) `KMS_VIDEO_Blank` does
   nothing but return FALSE; the caller then does not pause either.
   `int KMS_VIDEO_InternalPanel(void)` tells the caller which case it is.
3. **`emulator/kms/main.c`**: after `PLATFORM_Keyboard()` in the frame
   loop, when `KMS_INPUT_lid_closed && KMS_VIDEO_InternalPanel()`:
   `Sound_Pause()`, `KMS_VIDEO_Blank(TRUE)`, `Log_print("kms: lid closed,
   screen off, paused")`, then loop on `KMS_INPUT_WaitEvent(0.5)` until
   the lid opens; inside the loop call `KMS_LIBRARY_Poll()` and
   `KMS_HDEV_Poll()` once per wake-up (USB plug/unplug keeps working) and
   nothing else; discard key and gamepad input that arrives while closed
   (`INPUT_key_code = AKEY_NONE`, the joystick state is re-read next
   frame anyway). On open: `KMS_VIDEO_Blank(FALSE)`, `Sound_Continue()`,
   `Log_print("kms: lid open, screen on")`, and treat the frame like a
   menu frame (`menu_ran`: its time and input are not the game's). The
   System menu, Help and Library must not be left open across a lid
   close: if a menu is open when the lid closes, the menus' own key loops
   must notice `KMS_INPUT_lid_closed`; the simplest acceptable form is
   that a menu's wait loop returns to the frame loop (closing the menu)
   when the lid closes, say which you chose.
4. **init**: nothing. The power button (`tiny_power_button` → init) and
   SSH behave as before with the lid closed. The ctl socket is not
   involved.
5. **Docs**: README, a short "Closing the lid" paragraph under the System
   menu / power section (screen off, emulator paused, sound muted;
   Wi-Fi, SSH and FujiNet keep running, so a closed laptop still drains
   its battery slowly: shut it down from F8 or the power button for the
   night; an external monitor keeps the picture; real suspend is a later
   experiment). CHANGELOG under Unreleased. KNOWN-ISSUES: no suspend yet,
   a few watts with the lid closed; machines whose firmware handles the
   lid itself (turns the backlight off) see the same thing twice, fine.

## B. No camera, no Bluetooth

The ZenBook's `lsmod` shows `uvcvideo` (and `videobuf2_*`, `videodev`,
`mc`, `uvc`) and `btusb`, `btmtk`, `btintel`, `btbcm`, `btrtl`,
`bluetooth` loaded by modalias at boot. The appliance has no use for a
webcam or for Bluetooth (no stack, no pairing UI; gamepads and keyboards
are USB). Remove them from the modloop, so they cannot load anywhere.

1. **`scripts/mkimage.sh` `MODLOOP_PRUNE`**: add `drivers/media/usb`
   (whole: uvc and the remaining capture drivers), `drivers/bluetooth`,
   `net/bluetooth`. Then try `drivers/media` whole: the dependency check
   at the end of the prune (`modloop prune changed module dependencies`)
   tells you whether anything that must stay needs the media core
   (`snd-usb-audio` may need `mc` depending on the kernel config; USB
   sound cards and headsets must keep working). Prune as much of
   `drivers/media` as that check allows and report what stayed and why.
   Also `drivers/nfc` and `net/nfc` if present and not already pruned.
2. **`docs/specs/zenbook-modules.txt`** (the "must survive" list): remove
   the camera and Bluetooth names (`bluetooth btbcm btintel btmtk btrtl
   btusb mc uvc uvcvideo videobuf2_common videobuf2_memops videobuf2_v4l2
   videobuf2_vmalloc videodev` and any other that the prune now removes
   on purpose), with a one-line comment in the file if it supports
   comments (see how `check_names` reads it), otherwise in mkimage.sh.
3. **Firmware**: Bluetooth firmware that may still be on the modloop:
   check `qca/` (`nvm_*`, `rampatch_*`, `crbtfw*`, `crnv*`, `htbtfw*`,
   `htnv*`), `rtl_bt/`, `mediatek/BT_RAM_CODE_*`, `ar3k/`, `ti-connectivity/
   TIInit_*`, `intel/ibt-*` (already dropped) and `brcm/*.hcd` (already
   dropped). Drop what exists, in the prune block next to the Intel
   Bluetooth line, and report the sizes.
4. Both images: the x86_64 and the i686 (`ARCH=x86`) builds share the
   prune list and the check; both must build.
5. **Docs**: README "Hardware" section: one sentence that there are no
   webcam or Bluetooth drivers on purpose (Bluetooth gamepads and
   keyboards are not supported, USB ones are). CHANGELOG (modloop size
   before and after). KNOWN-ISSUES: Bluetooth input devices unsupported.

## Verification

1. Host `-Werror` build (`make -C build/atari800-host`) and
   `scripts/alpine-env.sh run scripts/build-emulator.sh` clean; both
   images build (`scripts/alpine-env.sh run scripts/mkimage.sh` and
   `ARCH=x86 scripts/alpine-env.sh run scripts/mkimage.sh`); the i686
   build's `zenbook-modules` check is non-strict and must still pass.
2. Lid in QEMU: there is no lid switch to close, but `uinput.ko` is in
   the modloop. Write a small static C tool in the scratchpad (built in
   the Alpine environment) that creates a uinput device with `EV_SW` /
   `SW_LID` named `Lid Switch` and sends `close` or `open` (one argument),
   loading `uinput` first (`modprobe uinput`). Put it on a debug copy of
   the x86_64 image with `mcopy` (mtools is there; the debug-image rules
   of earlier specs apply: a copy under a name outside the
   `build/atari-os-x86-*.img` glob, deleted afterwards). Boot it with the
   harness (`scripts/qemu-test.sh start bios`, `wait`, `serial`), run the
   tool with `close`: the log shows `kms: lid closed, screen off, paused`,
   the emulator's CPU use drops to about nothing (`top -bn1` over
   `serial`), a key typed does nothing; the screenshot is black if QEMU's
   bochs-drm has a DPMS property, else the log says it has none. Then
   `open`: `kms: lid open, screen on`, the picture is back (screenshot),
   typing works, the Hardware page's `Input lag` and `Wake` lines carry
   on without a wild sample. Repeat close/open five times quickly.
   Also: close the lid while the System menu is open and while the
   Library is open, then open it: back at the Atari, no crash.
3. Modules: in the same QEMU boot, `lsmod` has no `uvcvideo`, `btusb` or
   `bluetooth`; `find /lib/modules -name 'snd-usb-audio*'` still finds
   the USB sound driver (or whatever the dependency check made you keep,
   reported). `ls /lib/firmware/qca /lib/firmware/rtl_bt` and the others
   from B.3 are gone or were never there.
4. Sizes: modloop before and after (the mkimage log line `modloop
   modules: ... MB`), firmware before and after.
5. On the ZenBook (Claude deploys and asks Carlos to close the lid):
   screen off, log lines, back on open, the Low latency numbers after
   the wake unchanged (about 12 ms avg); `lsmod` free of the pruned
   modules; `dmesg` without new errors at boot.

## Report

Files changed; the uinput tool's path (scratchpad) and the exact
commands used for 2 and 3; screenshots' paths (`build/qemu-test/`);
which `drivers/media` parts stayed and why; the firmware files dropped
with sizes; the modloop size before and after; the menu-under-lid choice
in A.3; anything in this spec that turned out wrong.
