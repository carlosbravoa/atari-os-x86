# Spec: the Library (F9)

Status: agreed 2026-11-07, to implement. Owner of the design: Claude (Fable);
implementer: an Opus agent; review, QEMU/laptop verification and commits: Claude.

## 1. Purpose

A newcomer flashes the stick, boots, plugs a USB drive full of Atari files,
presses **F9**, and can **run** any of them or **copy** them onto the stick
so they stay. Someone exporting the other way (stick → USB) can do that too.
No typing of paths, no DOS knowledge. Everything is host-side: the Atari
itself is not involved (an in-Atari shell, ASH, comes later and sees the same
files through `H:`).

Non-goals for this iteration: artwork/metadata, multi-select, delete/rename
on the stick, save-state management, zip browsing beyond what atari800 does
natively, touch/mouse.

## 2. Files and volumes

On the stick (`/media` inside the appliance, `MEDIA` in `init/init.c`):

```
/atari/library/     the collection; any folder structure; copies land here
/atari/disks/       writable ATRs the user keeps (future "My disks" tab)
/atari/fujinet/SD/  FujiNet's SD folder
/atari/config/library.cfg   recent + favourites (see §6)
```

USB drives: `init` mounts every block partition that is not the boot stick
and not a loop device, on hotplug and at boot, under `/media/usb/<NAME>`
where `<NAME>` is the filesystem label if it has one (sanitised to
`[A-Za-z0-9_-]`), else the device name (`sdb1`). Filesystems: vfat, exfat,
ntfs3, read-write, vfat with `flush` (write-through), all with
`noatime,nosuid,nodev`. On the `remove` uevent (or when the device node
disappears) init unmounts (`MNT_DETACH`) and removes the directory. Volumes
that hold `/atari-os/modloop.sqfs` are never treated as USB drives (that is
a second atari-os stick). The partition the appliance booted from and the
Ventoy host partition (`HOSTFS`) are excluded.

The emulator discovers volumes by reading `/media/usb/` (no socket command
needed) and watches it with inotify so a newly inserted drive triggers the
border hint (§5).

Media file types the Library lists (case-insensitive extension):
`atr xfd dcm atx pro xex com exe bas lst car rom bin cas zip a8s` plus
`.gz` variants of atr/xfd. Everything else is hidden. Directories are
always shown. Hidden files (leading dot) are hidden.

## 3. Where it lives in the code

- New files `emulator/kms/library.c` and `emulator/kms/kms_library.h`
  (added to `src/Makefile.am` inside the `WITH_VIDEO_KMS` block via the
  existing patch `emulator/patches/0001-add-kms-video-and-alsa-sound-targets.patch`;
  regenerate the patch from the submodule with
  `git -C third_party/atari800 diff -- configure.ac src/Makefile.am src/ui.c`).
- Hotkey: **F9** opens the Library. In `emulator/kms/input.c`, `KEY_F9`
  currently returns `AKEY_EXIT`; it must instead set a request flag
  `KMS_LIBRARY_requested` (same pattern as `KMS_SYSMENU_requested`), handled
  in the main loop in `emulator/kms/main.c` right where the System menu is
  handled (Sound_Pause / run / Sound_Continue, `INPUT_key_code = AKEY_NONE`).
  Nothing else may map to `AKEY_EXIT` from a single key: shutdown is only
  via the menus (System menu "Shut down", F1 menu "Shut down the laptop")
  and the power button.
- F1 menu: the first entry becomes `Library` (accel "F9") and opens
  `KMS_LIBRARY_Run()`; "Run Atari Program" stays as the second entry.
  Done in `src/ui.c` under `#ifdef KMS`, same style as the existing
  `UI_MENU_MONITOR` hook; extend the patch file accordingly.
- Written like the rest of the port: C89 (the build uses
  `-ansi -pedantic -Werror`; no `//` comments, no declarations after
  statements, no variadic macros, no `long long`), musl-clean, no new
  libraries. Follow the look of `emulator/kms/sysmenu.c` (UI driver calls,
  `Log_print`, `Util_*` helpers). `PLATFORM_Keyboard()` must be the only way
  keys are read (the UI driver does that).
- `init/init.c` gets the USB volume mounting (§2) in the existing uevent
  handler (`uevent_read`) and at boot after the second coldplug. Keep the
  functions small and in the style of the file (logmsg, run(), exists()).
  Add `usb <n>` to the `status` reply (number of mounted USB volumes).

## 4. The screen

Built with `UI_driver->fSelect()` and friends, Atari look, title shows the
current source and path, for example `Library: /GAMES` or
`USB KINGSTON: /`. One `fSelect` call per view; rebuild the item list on
every change. Behaviour:

- Entries: `..` first (except at a source's root), then directories
  (suffix `/`), then files sorted case-insensitively. Files show the
  extension and size (`RIVER RAID.XEX   8K`), via the item suffix. Long
  names are truncated by the UI, that is acceptable.
- **RETURN** on a directory enters it; on a file opens the action popup
  (§4.1). **Backspace** (`UI_USER_DELETE` seltype) goes to the parent
  directory. **Esc** at a source root closes the Library; deeper, it goes
  up one level.
- **Tab** (`fSelect` returns -2) cycles the source: Library → each USB
  volume → FujiNet SD → back to Library. The first entry of every view is
  `[ Source: … TAB ]` as a non-selectable label so the key is discoverable.
- The Library root shows a `Recent` block before the listing: up to 8
  entries, most recent first, each selectable (RETURN opens the action
  popup for that path). Favourites are shown in the listing with a `*`
  prefix and also collected in a `Favourites` block after Recent.
- A `[ Filter: … ]` selectable entry (second line) asks for a substring via
  `fEditString`; while set, only matching files (and all directories) are
  shown and the title gets ` (filter: xxx)`. Empty string clears it.
- Volume disappears while browsing it → go back to the Library root with
  a message.

### 4.1 Action popup (RETURN on a file)

`fSelect(title=<file name>, UI_SELECT_POPUP, ...)` with:

1. `Run` – boot it (§4.2).
2. `Mount as D1:` – `SIO_Mount(1, path, FALSE)` without rebooting, then a
   short message. For `car/rom/bin` this item reads `Insert cartridge`
   (`CARTRIDGE_InsertAutoReboot`), for `cas` `Insert tape`
   (`CASSETTE_Insert`), for `xex/com/exe/bas/lst` it is hidden.
3. `Copy to Library` (hidden when already in the Library) /
   `Copy to USB …` (one item per mounted USB volume; hidden when the file
   is on that volume) / `Copy to FujiNet SD`.
4. `Favourite` / `Unfavourite`.
5. `Cancel`.

### 4.2 Run

`AFILE_OpenFile(path, TRUE /*reboot*/, 1, FALSE)`; on failure show
`Could not run: <name>` and stay. Before that, multi-disk detection: if the
file name matches `… (Disk 1)…`, `…_d1…`, `… disk1…`, `… side 1…`
(case-insensitive) and siblings with 2..4 exist in the same directory,
`SIO_Mount(n, sibling, FALSE)` for n = 2..4 after the open. Record the path
in Recent (§6), then leave the Library and return to the emulation (the
cold start already happened inside `AFILE_OpenFile`).

### 4.3 Copy

Destination keeps the path relative to the source root: `USB:/GAMES/X.XEX`
→ `library/GAMES/X.XEX`, creating directories. If the destination exists,
ask `Overwrite <name>?` with a yes/no popup. Copy in 64 KiB chunks; every
5 % call `UI_driver->fMessage("Copying… 42%", FALSE)` so the screen updates
(fMessage with waitforkey=FALSE just draws). On completion `fsync()` the
file, `sync()` once, and show `Copied. Safe to unplug.` (for USB
destinations) or `Copied.`. Errors show the `strerror` text. Never copy
onto the file itself; refuse to copy a directory (out of scope).

## 5. Plug-in hint

When a new entry appears under `/media/usb/` while the emulation runs,
call `KMS_VIDEO_SetHint("USB drive found: F9 = Library", 12.0)`. Implement
with an inotify watch on `/media/usb` polled from the Library module once
per frame (a hook `KMS_LIBRARY_Poll()` called from the main loop next to
the sysmenu handling). Also log `library: usb volume <name>`.

## 6. Persistence

`/atari/config/library.cfg` (fall back to `$HOME/library.cfg` when no
stick), plain text, one entry per line: `recent=<absolute path>` (newest
first, keep 8) and `fav=<absolute path>`. Loaded when the Library opens,
written after each change. Paths on volumes that are not mounted are kept
but shown with a `?` prefix in Recent and cannot be run until the volume is
back.

## 7. Build and verification (the implementer does all of this)

- Enable zip support in the emulator build: in `scripts/build-emulator.sh`
  change `--with-zlib=no` to `--with-zlib=yes`; `zlib-dev` is already in the
  Alpine env (`scripts/alpine-env.sh`'s BUILD_PKGS has it via build-base?
  If `configure` cannot find zlib, add `zlib-dev` to BUILD_PKGS and run
  `scripts/alpine-env.sh run apk add zlib-dev`). `libz.so.1` is already
  copied into the initramfs by `copy_deps`.
- Build: `scripts/alpine-env.sh run scripts/build-emulator.sh` (touch
  `third_party/atari800/configure` first if the Makefile predates the
  configure flags change) and
  `scripts/alpine-env.sh run sh -c 'SKIP_STAGE=1 EXTRA_CMDLINE="console=ttyS0,115200 atari.debug=1" scripts/mkimage.sh'`.
  Also compile on the host: `cd build/atari800-host && make -j16` must be
  warning-free (it uses `-Werror` too).
- Test disk: create `build/usb-test.img` (64 MB, MBR, one vfat partition
  labelled `GAMES`, built the way `scripts/mkimage.sh` builds the stick with
  `sfdisk`/`mformat`/`mcopy`) containing `GAMES/HELLO.XEX`, `DEMOS/A.ATR`,
  `README.TXT`, and a nested folder. For HELLO.XEX and A.ATR use files
  already in the atari800 tree or the FujiNet dist (`autorun.atr` is in
  `build/fujinet/dist/data/`, copy it); it only needs to be a valid image
  that boots to something. Add `QEMU_EXTRA` support to
  `scripts/qemu-test.sh` so extra QEMU arguments can be passed, and attach
  the test disk as a second USB stick:
  `-drive if=none,id=usb2,format=raw,file=build/usb-test.img -device usb-storage,bus=xhci.0,drive=usb2,removable=on`.
- With `scripts/qemu-test.sh` (`start`, `key`, `shot`, `serial`, see README
  "Headless testing") show, as PNG screenshots under `build/qemu-test/`:
  the border hint after boot, the Library root (Recent empty), the USB
  tab listing, the action popup, a copy result, and the Library root after
  the copy showing the copied folder and the Recent entry after a Run.
  Check `/run/atari800.log` for `library:` lines and no errors.
  QEMU with KVM is available (`/dev/kvm` writable), so a boot takes ~15 s;
  wait for `pidof atari800` over serial before sending keys.
- Do not commit. Report: files changed, what was verified with which
  screenshots, anything in this spec that turned out wrong or impossible
  and what you did instead.

## 8. Out of scope but keep the door open

- A `My disks` tab over `/atari/disks/` with "new blank disk" (needs a
  DOS-less blank ATR generator; later, with ASH).
- ASH as the in-Atari shell over `H1:`=library `H2:`=USB `H3:`=FujiNet SD
  `H4:`=disks (atari800 `-H1 … -H4` options); the Library's volume paths
  are what those will map to.
