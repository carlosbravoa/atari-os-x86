# Spec: two menus, a Help key, and a keyboard-type switch

Status: from Carlos's session 2026-11-07 ("the overlap of functionalities
and the lack of completion of a full use case … manage everything with two
menus … one extra help page with all the shortcuts. It is impossible to
guess … a switch for keyboard type"). Design: Claude (Fable); implementer:
an Opus agent; QEMU and laptop verification: Claude. Rules as in
`docs/specs/library.md` §3/§7 (patch regeneration, `-Werror`, nothing
outside `emulator/`, `third_party/atari800` only through the patch, docs
updated in the same change). The patch now covers `configure.ac
src/Makefile.am src/atari.c src/cfg.c src/sio.c src/ui.c src/ui_basic.c`
(ui_basic.c: bit 7 of a character = inverse video in the menus).

## 0. Decisions (settled with Carlos)

1. **F1 = Help**, one paged screen with every key. The stock atari800 menu
   stops being a front door: it moves to the bottom of F8 as
   "Advanced: emulator menu...". Nothing is removed from it.
2. **F8 = System**, regrouped into submenus: 19 flat lines become 15 (16
   with Install), and every setting lives in exactly one place. **Shut down stays top-level.**
3. **F9 = Library** gains a Drives line and "Mount as D2..D4", so the
   day-to-day disk use cases (swap disk 2 mid-game, see what is mounted,
   eject one drive) finish inside the Library.
4. **Keyboard type switch**: PC (default) or Atari (positional), in F8 >
   Controls, saved in the config, shown on the Help page.

The three menus answer three questions: F1 "which key?", F8 "how does the
machine behave?", F9 "what do I play?".

## 1. Today's overlap (why)

- Display: F8 "Display..." and F1 "Display Settings" (NTSC filter in both).
- Mounting: F1 Disk/Cartridge/Tape Management, the Library popup, and
  "Eject all" in F8, in the Library popup and in F1.
- Reset: F1 warm/cold, F8 "Restart emulator", F8 "Eject all, back to",
  Library "Eject all & reboot".
- Volume, TV system and BASIC on/off exist in F8 and again inside F1
  System/Sound Settings with other words.
- Nothing on screen says what F6, F7, F11, F12 or the backtick key do.
- Shift+arrow types `- = + *` (verified in QEMU): neither what a PC user
  nor what an Atari user expects.

## 2. Keys

| Key | Does |
|---|---|
| F1 | Help (§3) |
| F2 / F3 / F4 | OPTION / SELECT / START |
| F5, Shift+F5 | warm / cold reset |
| F6, F7 | HELP, BREAK |
| F8 | System menu (§4) |
| F9 | Library (§5) |
| F10, Shift+F10 | screenshot, interlaced screenshot |
| F11 | arrows: cursor keys <-> joystick 0 |
| F12 | turbo on/off |
| ` or Win | Atari key (inverse video) |
| Home, Ins, Del | CLEAR, insert, delete (Shift: line) |
| Caps Lock | CAPS |
| Shift+arrow | cursor key with SHIFT (as Shift+Ctrl+arrow on an Atari); no more `- = + *` |
| Arrows + left Alt | joystick 0 after F11; numpad 8/2/4/6 + 0 always |

Gamepad: D-pad/left stick = joystick, A/triggers = fire, Start/Select/X =
START/SELECT/OPTION, B = Esc in menus, **Guide/Mode button = System menu
(F8)**, no longer the stock menu (a gamepad-only user reaches the Library
through F8's first line and can shut down).

The boot hint under the splash becomes `F1 = Help   F8 = System   F9 =
Library` (same once-until-opened rule; opening any of the three clears it).

## 3. F1: Help

New `emulator/kms/help.c` / `kms_help.h`, `KMS_HELP_Run()`, shown with
`UI_driver->fInfoScreen("Keys", page)` (paged, Esc closes) in the same
style as "Hardware status". `AKEY_UI` from F1 is intercepted in `input.c`
(like F8/F9: `KMS_HELP_requested`, served in `main.c`'s frame loop where
the other two are). Content, 38 columns max per line:

```
F1  this page      F8  System menu
F9  Library        Esc back / close
F2  OPTION   F3  SELECT   F4  START
F5  reset    Shift+F5  power-cycle
F6  HELP     F7  BREAK   F12 turbo
F10 screenshot (Shift: interlaced)
F11 arrows = cursor keys / joystick
` or Win  Atari key (inverse video)
Home CLEAR  Ins/Del insert/delete
Caps Lock  CAPS       Tab  TAB
Shift+arrows  Shift+cursor
Alt+arrows  joystick 0 after F11
Numpad 8 2 4 6 move, 0 fire (always)

Keyboard type: PC            (F8 > Controls)
  keys type what their caps say
Gamepad: stick/D-pad move, A fire,
  Start/Select/X = START/SELECT/OPTION,
  B back, Guide = System menu

Library (F9): Tab next source,
  RETURN open/run, Backspace up,
  Esc close, Filter narrows names
Menus: arrows, RETURN, Esc back
Power off: F8 > Shut down, or the
  power button. Never just unplug.
```

When the keyboard type is Atari, the "Keyboard type" line reads
`Keyboard type: Atari (positional)` and a second page lists the positional
table of §6. Also reachable as F8 > "Help: keys and menus" (same function).

## 4. F8: System

Top level, in this order (`sysmenu.c`):

```
System                                        F8
  Library                                     F9
  Boot into: BASIC                          >  BASIC / No BASIC (self test) / ASH / FujiNet CONFIG
  Eject all, back to BASIC
  Display...                                >  §4.1
  Sound: 80%                                   volume slider (as today)
  Controls...                               >  §4.2
  Network: <ssid or state>                  >  §4.3
  Help: keys and menus
  Hardware status
  Install onto this laptop's disk...           (hidden unless stick + internal disk, as today)
  Save logs to the stick
  Restart emulator
  Reboot laptop...                              (UEFI: popup "Straight back into Atari" (BootNext) / "Normal reboot (boot order, other OS)")
  Shut down
  Advanced: emulator menu...
  Back to Atari                                Esc
```

Dropped from the top level, not lost: Brightness and TV system go into
Display; Wi-Fi, IP, SSH and SSH password go into Network. The `IP ...
Battery ...` label line goes (both are in Hardware status; Network shows
the IP). "Shut down" must remain reachable the same way from a gamepad
(B = back, A = select) and its position is documented for the QEMU notes
(README "QEMU" section: count the downs from the top and update the
`Shut down` recipe and `scripts/qemu-test.sh` comments).

### 4.1 Display...

Existing submenu plus two moved items, in this order: `Scaling`, `Smoothing`,
`Scanlines`, `Borders`, `TV look`, `TV system: NTSC/PAL` (the existing
popup, live switch, saved), `Brightness: 80%` (slider), `Back`. Remove the
stock F1 "Display Settings" NTSC item's claim to be the only place:
nothing to change in ui.c, both write the same `ARTIFACT_NTSC` option.

### 4.2 Controls...

```
Controls
  Arrows: cursor keys | joystick 0        (same state as F11; live)
  Keyboard type: PC | Atari               (§6; live; saved KMS_KEYBOARD)
  Gamepad 1 drives: joystick 0 | joystick 1   (swap; live; saved KMS_PAD_SWAP=0|1)
  Gamepad buttons                          (info page: the mapping of §2)
  Back                                     Esc
```

"Gamepad 1 drives" swaps pads[0] and pads[1] in the stick/trigger
synthesis only; menu navigation keeps following pads[0] physically.
`KMS_INPUT_ReadConfig/WriteConfig` already exist in `input.c`: add the two
options there.

### 4.3 Network...

```
Network
  Wi-Fi: <ssid | state>           >  the existing Wi-Fi submenu (scan, pick, password, forget)
  IP: 192.168.0.221                  label (or "no network")
  Remote access (SSH): on | off      as today (toggle; info page when turned on)
  SSH password: set | not set        as today
  Back                               Esc
```

### 4.4 Advanced: emulator menu...

Calls `UI_Run()` (the stock menu) from inside the System menu, exactly as
F1 did. In the stock menu (ui.c patch, `#ifdef KMS`): keep "Library F9",
rename the F8 entry to "System menu" with accelerator "F8", keep "Shut
down the laptop". Everything else stays untouched so save states,
recording, machine type, cartridge types and the monitor remain available.

## 5. F9: Library

### 5.1 Drives line

A third header line under Source and Filter, on every listing (all
sources, all folders):

```
[ Drives: D1 Game (Disk 1).atr  D2 Game (Disk 2).atr ]
```

Content: the slots that are not empty, in the order D1 D2 D3 D4 Cart Tape,
each as `Dn <basename>` (Cart: `Cart <basename>`, Tape: `Tape <basename>`),
truncated to fit 38 columns, ending in `...` when more follow; when all are
empty: `[ Drives: nothing mounted ]`. RETURN on it opens the **Drives**
popup:

```
Drives
  D1: Game (Disk 1).atr
  D2: Game (Disk 2).atr
  D3: empty
  D4: empty
  Cartridge: empty           (Cartridge: ASH.CAR when the ASH route put it in)
  Tape: empty
  Eject all & reboot         (only when something is mounted)
  Cancel
```

RETURN on a mounted slot asks `Eject D2:?` (Yes/No) and dismounts it
(`SIO_Dismount`, `CARTRIDGE_Remove`, `CASSETTE_Remove`); no reboot.
RETURN on an empty slot shows "Pick a file and choose Mount as D3:"
(message) and returns to the listing. Ejecting the cartridge or D1 does
not change the boot target; the next cold start follows `kms/boot.c`'s
rule as today.

### 5.2 File popup

For a disk image: `Run`, `Run with BASIC`, `Mount as D1:`, `Mount as
D2:`, `Mount as D3:`, `Mount as D4:`, then the copy items, `Favourite`,
`Eject all & reboot` (when something is mounted), `Cancel`. "Mount as Dn:"
mounts without a cold start and reports `Mounted as Dn:`; mounting into
D1 behaves as today (the next cold start boots it). Multi-disk sets: `Run`
keeps filling D2..D4 automatically as today; "Mount as Dn:" mounts only
the chosen file. Cartridge and tape items unchanged ("Insert cartridge" /
"Insert tape" means the next cold start boots it).

### 5.3 Unchanged

Sources (Tab), Recent, Favourites, Filter, copy items, paging, Esc rules.

## 6. Keyboard type

`KMS_INPUT_keyboard_pc` (TRUE default; config `KMS_KEYBOARD=pc|atari`).
Letters, digits unshifted, function keys, arrows, Esc, Return, Tab,
Backspace, Space, Caps Lock, Home/Ins/Del, numpad, and every Ctrl
combination are identical in both modes.

**PC (natural)**: what the key cap says is what the Atari gets (today's
`symbol_map`). `{ } ~` and backtick-as-character have no Atari key (`
is the Atari/inverse key). Shift+arrows = cursor + SHIFT bit (§2).

**Atari (positional)**: the PC key sends the Atari key in the same physical
position, with the modifiers as pressed, so the Atari's own key caps apply
(Shift+2 = `"`, Shift+7 = `'`, Shift+8 = `@`, Shift+6 = `&`, Shift+- is
CLEAR only via Home, etc.):

| PC key | Atari key (scancode, plain / shifted via AKEY_SHFT) |
|---|---|
| 1..0 | 1..0 (`AKEY_1`.. with the SHIFT bit when shifted) |
| - | `<` (AKEY_LESS; shifted AKEY_CLEAR) |
| = | `>` (AKEY_GREATER; shifted AKEY_INSERT_CHAR) |
| [ | `-` (AKEY_MINUS; shifted AKEY_UNDERSCORE) |
| ] | `=` (AKEY_EQUAL; shifted AKEY_BAR) |
| ; | `;` (shifted `:`) |
| ' | `+` (AKEY_PLUS; shifted AKEY_BACKSLASH) |
| \ | `*` (AKEY_ASTERISK; shifted AKEY_CARET) |
| , . / | same (shifted `[ ] ?`) |
| ` or Win | Atari key (inverse) |
| Ctrl+arrow and plain arrow | cursor keys (kept: PC users expect arrows to move) |

Implement as a second table `symbol_map_atari[]` chosen by the flag in
`translate()`; `letter_akey` and the arrow handling are shared. The Help
page's second page prints this table from the same data (one static table
of `{pc label, atari plain, atari shifted}` strings, so Help and code
cannot drift).

## 7. Docs

- README: "Keys" table = §2; the F8 and F9 sections rewritten to the new
  trees; a "Help (F1)" paragraph; the boot-hint sentence; the QEMU "Shut
  down" recipe (down count) and any other menu-position recipes.
- `scripts/flash.sh` end-of-run text (line "3. F8 = System menu ...")
  mentions F1 = Help.
- `.github/release-template.md`: one line "F1 shows every key".
- CHANGELOG under Unreleased; KNOWN-ISSUES: `{ } ~` cannot be typed in PC
  mode (no Atari key); Alt+letter accelerators of the stock menu are not
  bound (never were).

## 8. Verification (Claude, QEMU; then the laptop)

Build: `scripts/alpine-env.sh run scripts/build-emulator.sh`, host
`make -C build/atari800-host -j16` clean under `-Werror`, then mkimage and
a debug-imaged copy (`build/atari-os-test.img`).

1. F1 at READY shows the Help page; Esc returns; the boot hint reads
   `F1 = Help   F8 = System   F9 = Library` and clears after any of them.
2. F8: 15 lines as in §4; Display has TV system and Brightness; Controls
   has the four items; Network has Wi-Fi/IP/SSH/password; "Advanced"
   opens the stock menu whose F8 entry says "System menu"; Esc chains
   back to the Atari. Shut down powers QEMU off ("reboot: Power down").
3. F9: Drives line present at every level; mount a disk as D2 from the
   popup, Drives popup shows it, Eject D2 removes it; Run of a `(Disk 1)`
   set still fills D2..; "Eject all & reboot" from the Drives popup returns
   to the boot target.
4. Keyboard: PC mode `PRINT "2@7'6^-_;:=+[]"` echoes the same string
   (today's baseline screenshot `build/qemu-test/keys1.png`); Atari mode:
   QEMU `sendkey shift-2` prints `"`, `shift-8` prints `@`, `minus` prints
   `<`, `apostrophe` prints `+`; Shift+Up moves the cursor (no `-`).
   Setting survives a restart (`KMS_KEYBOARD=atari` in the config).
5. Gamepad: with QEMU `-device usb-gamepad`? not available; verify on the
   laptop with a USB pad: Guide opens System, B backs out, swap works.
6. Settings survive an emulator restart (kill atari800 on the debug
   image; init restarts it). Note `kill -USR1 1` reboots the machine, it
   is not an emulator restart.
