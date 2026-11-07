# Spec: ASH, the shell inside the Atari

Status: agreed 2026-11-07 ("yes, ash is second level, inside atari";
"Let's do both"). Design: Claude (Fable); implementer: an Opus agent;
review, QEMU/laptop verification and commits: Claude. Same rules as
`docs/specs/library.md` §3 and §7 and `docs/specs/polish-1.md` (C89
`-ansi -pedantic -Werror`, musl, no new libraries, don't commit, report).

## 1. What ASH is and the key finding

ASH (https://github.com/carlosbravoa/atari800-atari-shell, Carlos's
project) is a Unix-style shell for the Atari 8-bit that replaces the DOS
menu: `ls`, `cd`, `cat`, `cp`, `run`, an editor, a resident `D:` handler.
Its `h:` device is **not** atari800's `H:` CIO device: it is the Tang Nano
20K board's SIO file device **$72** ("H: hard-drive device", `/HDD` on the
board's SD card), and its `sd:` device (whole card, `mount`, `cart`,
`reboot`) is the same SIO device's control XIOs. ASH talks to it with plain
SIO frames through `SIOV` (`k_hcmd` in `kernel.s`: DDEVIC $72, DUNIT 1,
DAUX2 = handle, `sd_xio` in `sd.s`: command $58, AUX1 = function, 64-byte
frame).

So the right way to put ASH inside our Atari is to **emulate that SIO
device in the emulator**, backed by the stick's folders. ASH then runs
unmodified (the ASH-TN build), its `sd:` becomes the stick plus USB drives,
`mount`/`cart`/`reboot` act on atari800. No fork needed for this phase.

Reference implementation: `firmware/firmware.c` in
https://github.com/carlosbravoa/atari800_tang_nano20k (functions
`sio_hdd`, `hdd_open/read/write/close/status/delete/rename/path`,
`hdd_list`, `hdd_openr`, `hdd_statuslist`, `sd_defer`, `sd_openw`,
`sd_delete`, `sd_rename`, `sd_mkdir`, the `bridge_req` execution in the
main loop). A copy of the relevant parts is in §3 below; when in doubt,
clone that repo (blobless) into the scratchpad and read the original. The
ASH side is `kernel.s` (`k_hcmd`, `k_sio`, `pick_drive`), `fileops.s`
(h: streams) and `sd.s` in the ASH repo.

## 2. Deliverables

1. **Submodule** `third_party/atari800-atari-shell` =
   https://github.com/carlosbravoa/atari800-atari-shell (default branch).
   We use its prebuilt `release/ASH.CAR` (Williams 32 KB cartridge with a
   16-byte CART header), `release/ASH-TN.XEX` and `release/ASH-TN.ATR`;
   Alpine has no cc65, and no ASH change is needed in this phase.
2. **`mkimage.sh`**: copy the three files to the stick as
   `/atari/ash/ASH.CAR`, `/atari/ash/ASH.XEX`, `/atari/ash/ASH.ATR`, and
   create the folder `/atari/hdd/` (ASH's `h:`), with a short
   `/atari/hdd/README.TXT` (ATASCII line ends, `cat`-able) saying what
   `h:` and `sd:` are here. Also seed the library with nothing new.
3. **The device**: `emulator/kms/hdev.c` + `kms_hdev.h` (§3), hooked into
   `SIO_Handler()` in `src/sio.c` with a 5-line `#ifdef KMS` block
   (extend `emulator/patches/0001-….patch`; regenerate from the submodule
   with `git -C third_party/atari800 diff -- configure.ac src/Makefile.am src/ui.c src/sio.c`).
   Deferred actions are executed from the main loop by `KMS_HDEV_Poll()`
   (called next to `KMS_LIBRARY_Poll()` in `emulator/kms/main.c`).
4. **Boot target** (§4): `system.conf` gets `boot=basic|ash|fujinet`
   (replacing the boolean `fujinet=`; read the old key as `boot=fujinet`
   for compatibility, write only the new one). Control socket: `boot
   basic|ash|fujinet` (keep `fujinet on|off` as aliases). `status` reports
   `boot <target>`. F8 System menu: the "FujiNet boot" toggle becomes
   `Boot into: BASIC / ASH / FujiNet CONFIG` (a popup; choosing restarts
   the emulator like the toggle did). The hardware page shows `Boot:
   <target>`. README: a new section "ASH" (what it is, link to the repo,
   `h:`/`sd:` mapping, the `boot` setting) and the keys table unchanged.
5. **Library**: `.car` files already run via the action popup; nothing to
   add, but verify `/atari/ash/ASH.CAR` runs from the Library (Run =
   insert cartridge + reboot).

## 3. The $72 device, exactly

### 3.1 Hook

In `SIO_Handler()` (sio.c), after the disk branch and before the cassette
branch:

```c
#ifdef KMS
	else if (MEMORY_dGetByte(0x300) == 0x72)
		result = KMS_HDEV_Handler(cmd, MEMORY_dGetByte(0x30a), MEMORY_dGetByte(0x30b), data, length);
#endif
```

`KMS_HDEV_Handler` returns `'C'` (complete), `'E'` (error) or `'N'` (NAK:
malformed/unknown), and moves data between the Atari's memory
(`MEMORY_CopyToMem` / `MEMORY_CopyFromMem` at `data`, `length` bytes) and
the host. The SIO patch is instant; no timing to emulate. Note that
`-netsio` turns the SIO patch off (atari.c), so in `boot=fujinet` the
device is absent and ASH hides `h:` (as on a real Atari without the
board); that is accepted for this phase (§6).

### 3.2 State

Two handles (`aux2 & 3`, only 0 and 1 are used by ASH), each either free,
a file (`FILE *` plus mode), or a *text listing* (one shared 1 KB buffer
with a read position, exactly one listing at a time, like the firmware).
`sd_rw` flag (off at every emulator cold start: reset it in `Atari800_Coldstart`
path, e.g. by checking a frame counter/coldstart hook; simplest: reset
when `KMS_HDEV_Poll` sees `Atari800_coldstart_done`-style change, or hook
`CARTRIDGE`/`Atari800_Coldstart` via the existing `KMS` patch; pick what
is least invasive and say which).

### 3.3 Paths

- `h:` names (OPEN/XIO 32/33) go through `hdd_path`: uppercase, 8.3
  discipline (one dot, never leading), only `A-Z 0-9 _ -`, `:` resets
  (tolerates a leading `H:`/`H1:`), everything else dropped, so no
  traversal. Resolved under `MEDIA_USER "/hdd/"` → `/media/atari/hdd/NAME.EXT`
  (fall back to `$HOME/hdd/` when no stick). FAT is case-insensitive so
  opening works whatever case the file has on disk.
- `sd:` paths (XIO 40–52) are absolute from a **virtual root**:
  `/atari/…` → `/media/atari/…`, `/usb/…` → `/media/usb/…`; the root
  listing contains exactly `d` entries `atari` and `usb` (the latter only
  if `/media/usb` exists). Anything else at the root → "cannot open" (-2).
  Normalise: collapse `//`, reject any `..` component (→ error). Leading
  `/` optional (FatFs accepted both). The stick's `atari-os/`, `EFI/`,
  `limine*`, `vmlinuz`, `initramfs` are therefore never visible.

### 3.4 Commands (device $72)

- **OPEN $4F**: aux1 = mode (4 read, 8 write/truncate, 9 append, 6
  directory), aux2 = handle; 64-byte name frame (NUL-terminate at 63).
  Mode 6: the listing of `h:` = for every regular file in `/atari/hdd/`
  whose name fits 8.3 after uppercasing (skip the rest, skip dot-files and
  directories): `NAME.EXT` left-justified to 14 columns, then the decimal
  size, then `$9B`; then `END OF DIRECTORY` `$9B`. Stop adding entries when
  the buffer would overflow (keep 20 bytes spare). Mode 8/9 create
  `/atari/hdd/` if missing. Return -3/-4 style errors → `'E'`.
- **READ $52**: aux1 = len (1..128, 0 → 128); copy exactly len bytes to
  the Atari; fewer available = `'E'` (ASH sizes reads via STATUS avail;
  short reads are an error by design). Works on files and listings.
- **WRITE $57**: aux1 = len; take len bytes from the Atari; `'E'` on
  write failure or if the handle is not a write-mode file.
- **CLOSE $43**: close (fflush+fclose), free the handle. Always `'C'`
  unless fclose fails.
- **STATUS $53**: 4 bytes `{err, flags, avail_lo, avail_hi}` to the
  Atari: err = 1 OK or 133 when the handle is free (still `'C'`: ASH's
  boot probe `pick_drive` counts any successful STATUS as "h: present");
  flags = 1 | (avail == 0 ? 2 : 0); avail = bytes left in the file or
  listing, capped at 65535.
- **XIO $58**, aux1 = function, 64-byte frame:
  - 32 rename `"OLD,NEW"` on `h:`; 33 delete on `h:`.
  - 40 LIST: frame = sd path (bytes 0..61, NUL), bytes 62..63 = start
    index (little-endian). The handle becomes a listing: for each entry
    from `start` on (skip dot-files): `d` or `f`, space, size right-aligned
    in 8 columns (spaces for directories), space, name cut to 27 chars,
    `$9B`; then `END$9B`, or `MORE$9B` when the buffer filled first (ASH
    re-lists with start = entries received). Sort entries: directories
    first, then files, both case-insensitive (the board shows FAT order;
    sorted is better and harmless).
  - 41 MOUNT: frame[0] = drive `'1'..'4'`, frame+1 = sd path. Deferred.
    `.xex` (case-insensitive) → `AFILE_OpenFile(path, TRUE, 1, FALSE)`
    (that is "run as D1: and cold boot", the board's `bridge_req=1`);
    anything else → `SIO_Mount(drive, path, FALSE)` (no reboot).
  - 42 UNMOUNT: frame[0] = drive → `SIO_Dismount(drive)`. Deferred.
  - 43 CART: frame = sd path → `CARTRIDGE_InsertAutoReboot(path)`; on
    success the cart name is remembered for STATUS. Deferred.
  - 44 BOOT: frame[0] `'C'` → `Atari800_Coldstart()`, `'W'` →
    `Atari800_Warmstart()`. Deferred.
  - 45 STATUS listing: `d1: <name>` … `d4: <name>` (basename of the
    mounted image from `SIO_filename[]`, `-` when none), `cart: <name|->`,
    `xex: 0|1` (1 while a binary load is active or was the last mount of
    D1 via 41), `boot: basic|ash|fujinet` (the current boot target, read
    from the `status` socket reply once at startup and after each `boot`
    change; the device gets it via `KMS_HDEV_SetBootTarget()` called from
    sysmenu when the user changes it), then `END`.
  - 47 OPENR: open any sd path read-only on the handle (directories →
    error), then READ/STATUS/CLOSE as a mode-4 file. This is how ASH
    `cat`s, `cp`s from and `run`s files on `sd:`.
  - 48 WRITEMODE: frame[0] = 0 ro, 1 rw, 2 query (handle becomes a
    listing `ro`/`rw` + `END`).
  - 49 OPENW (create/truncate for writing), 50 DELETE (files and empty
    dirs), 51 RENAME (frame = old NUL new NUL, within sd:), 52 MKDIR: all
    refused (-1 → `'E'`) unless `sd_rw`; all go through the **guard**
    (§3.5).
  - Unknown function → `'N'`.
- Unknown command → `'N'`. (The board stays silent; with the SIO patch a
  NAK is the equivalent and keeps ASH's error path short.)

Deferred actions are queued (one at a time; a second request before the
first ran → `'E'`) and executed in `KMS_HDEV_Poll()` on the next frame,
never inside `SIO_Handler`. Log every action with `Log_print("hdev: …")`.

### 3.5 Write guard

Writes under `sd:` are allowed only under `/atari/…` and `/usb/…` (by
construction of the virtual root), and refused (-3) for: anything under
`/atari/ash/` and `/atari/config/`, any file currently mounted on D1–D4
(`SIO_filename[]` compared by `realpath`), the inserted cartridge
(`CARTRIDGE_main.filename`), and any file open on the other handle for
writing. `h:` writes are confined to `/atari/hdd/` by `hdd_path`.

## 4. Boot target plumbing

- `init/init.c`: `conf.boot` enum {basic, ash, fujinet}; `boot=ash` adds
  `-cart /media/atari/ash/ASH.CAR` to the emulator argv (atari800 reads
  the type from the CART header). `boot=fujinet` is today's `-netsio`
  path. `boot basic|ash|fujinet` on the socket saves and restarts the
  emulator (reuse the `fujinet on|off` code path; keep those two strings
  working as aliases). `status`: `boot <target>` in addition to the
  existing `fujinet on|off` line (keep it so the current menu still parses
  during the transition; remove nothing else).
- `emulator/kms/sysmenu.c`: the first item becomes `Boot into: <target>`
  with a popup of the three choices; selecting a different one sends
  `boot <x>` and shows `Restarting…`. Missing `ASH.CAR` → the ASH choice
  shows `ASH (not installed)` and is refused with a message.
- Splash hint and KNOWN-ISSUES untouched.

## 5. Verification (QEMU, KVM, `scripts/qemu-test.sh`)

Add to `build/usb-test.img` (via `scripts/mk-usb-test.sh`) a folder
`DISKS/` with a second ATR so `mount d2:` has something to mount.
Screenshots under `build/qemu-test/`, log lines from `/run/atari800.log`:

1. Boot `boot=ash` (set via the socket: `scripts/qemu-test.sh serial
   'printf "boot ash\n" | nc -U /run/atari.ctl'` or edit system.conf on a
   copy; say which): ASH prompt appears; `df` shows `h:`; `ls h:` lists
   README.TXT; `cat h:readme.txt` shows the text.
2. `ls sd:/` shows `atari/` and `usb/`; `ls -l sd:/atari/library`; `ls
   sd:/usb/GAMES`.
3. `run sd:/usb/GAMES/GAMES/HELLO.XEX` (or the right path) starts the
   program (screenshot of it running); RESET returns to ASH (cartridge
   RESET hook) — screenshot.
4. `mount d2: sd:/usb/GAMES/DISKS/X.ATR` then `mount` (status listing)
   shows `d2: X.ATR`; `umount d2:`.
5. `sd rw`, `mkdir sd:/atari/library/FROMASH`, `cp h:readme.txt
   sd:/atari/library/FROMASH/` then `ls` it; then `cp sd:/atari/library/FROMASH/README.TXT h:copy.txt`.
   Negative: `rm sd:/atari/ash/ASH.CAR` must be refused.
6. `edit h:note.txt`, type a line, Ctrl-S, Ctrl-Q, `cat h:note.txt`.
7. `basic` drops to BASIC READY; `reboot` from ASH cold-boots back into
   ASH.
8. `boot basic` from the F8 menu restarts into BASIC; F8 again, `Boot
   into: ASH` → ASH. Screenshots of the popup.
9. Library: Run on `/atari/ash/ASH.CAR` from the Library tab boots ASH.
10. `boot fujinet` still boots CONFIG; in that mode `df` in ASH (if you
    load ASH.XEX from a disk) shows no `h:` — just confirm no crash.

Also run the host build (`build/atari800-host`, `-Werror`), and `init`
builds static. Typing into QEMU: `scripts/qemu-test.sh key` sends qemu
`sendkey` names; ASH needs lowercase letters, `:`, `/`, `-` and RETURN,
so check that the harness can send `shift-semicolon`, `slash`, `minus`
(extend the `key` helper if needed and document it in README "Headless
testing").

## 6. Phase 2 (not now, keep the door open)

- **FujiNet mode**: `-netsio` disables the SIO patch, so the $72 device
  would need a byte-level implementation on the POKEY serial path (or in
  FujiNet-PC as a virtual device). Decide after phase 1 is in use.
- **The mailbox**: ASH-TN's page-6 mailbox (`tn.s`, `net.s`) lets a "PC"
  serve `date`, `wget URL`, `tnfs://` and drive the shell. Here the PC is
  the host itself: the emulator can read/write `MEMORY_mem` directly, so
  `date` (host clock), `wget` (via the host network) and a Claude/SSH
  remote-control path (`ashterm`-style) are a small, high-value addition.
- A `My disks` tab in the Library over `/atari/disks/` with blank-ATR
  creation, now that ASH can `format` and write them.
