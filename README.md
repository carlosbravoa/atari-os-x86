# atari-os-x86

Turn an old x86 laptop into an Atari 8-bit computer. Plug in the stick,
power on, get `READY`.

This is not a Linux distribution with an emulator on top. The Linux kernel is
used as a driver layer and nothing else: there is no systemd, udev, shell,
display server, sound server or SDL on the stick. What runs is PID 1 (a small
supervisor), the emulator, and later FujiNet-PC with the network helpers it
needs.

Inspired by [bma8](https://github.com/serkantanriverdi/bma8) (bare-metal Atari
on Raspberry Pi) and [AltirraSDL](https://github.com/ilmenit/AltirraSDL).

## Install

Download `atari-os-x86-<version>.img.xz` from the latest
[release](../../releases/latest) (and `flash.sh` if you are on Linux or
macOS), then write it to a USB stick or SD card of 1 GB or more. Writing
erases the stick. Pick one:

- **Windows**: [balenaEtcher](https://etcher.balena.io/) writes the
  `.img.xz` as it is; [Rufus](https://rufus.ie/) works too, on the
  unpacked `.img` (7-Zip), in **DD mode**.
- **Linux and macOS**: `flash.sh` only ever offers USB sticks and SD cards
  (never a disk holding `/`, `/home`, swap and the like, nothing over
  512 GiB), makes you type the device name and `yes`, then writes and reads
  the stick back to verify it:

  ```sh
  bash flash.sh atari-os-x86-v0.1.0.img.xz
  ```

  `--dry-run` shows the disks and the plan without writing; `--help` for
  the rest. macOS needs `xz` (`brew install xz`) for the `.xz` file.
- **Ventoy**: `unxz` the file and copy the `.img` onto the Ventoy stick
  like any other image; pick it from the Ventoy menu.

Then boot the laptop from USB: open its boot menu while it starts (usually
F12; F9 on HP, Esc or F8 on ASUS) and pick the stick. On UEFI machines
Secure Boot must be off. The Atari comes up at `READY`; **F8** is the System
menu, **F9** the Library.

The stick stays an ordinary, writable FAT32 drive: your library, settings
and FujiNet's SD folder live in its `atari/` folder, and you can add files
from any computer. The image is 1 GiB; the rest of a bigger stick stays
unused for now.

### Install onto the laptop's disk

A laptop that is going to be an Atari and nothing else can run it from its
own disk, without the stick. Boot the stick on that laptop, then **F8 →
Install onto this laptop's disk...**. The item is only there when the
system runs from a stick (or an image file on one, Ventoy) and an internal
disk of 4 GB or more was found (SATA, NVMe or eMMC; not USB drives, not SD
cards, not the stick itself).

1. With more than one internal disk, pick one (model and size).
2. A page says what happens: **everything on that disk is erased**. The
   whole disk becomes one FAT32 partition; nothing else can be kept, so
   this is for machines whose old system is not wanted any more.
3. Type `ERASE` (capitals) and RETURN. Anything else cancels and nothing
   is written.
4. Partitioning, formatting, copying the system (with a percentage),
   installing the boot loader and, on UEFI machines, registering with the
   firmware take a minute or two. Then remove the stick and press RETURN:
   the laptop reboots into the Atari from its disk.

What gets copied is the stick's FAT as it is: the system, `atari/` with
your library, settings and FujiNet SD. The disk boots on BIOS (Limine in
the MBR, partition marked active) and UEFI (`\EFI\BOOT\BOOTX64.EFI`, plus a
firmware boot entry "Atari"). **The installer puts that entry first in the
machine's UEFI boot order.** It is the only place this project changes the
boot order, and only because you have just typed ERASE for a disk that will
hold no other system; an "Atari" entry from an earlier install is reused.
Secure Boot must stay off (Limine is unsigned).

The installed copy's `limine.conf` carries `atari.media=<volume serial>`,
so with the stick still plugged in the system uses the copy it booted from
and leaves the stick alone (it shows up as "another atari-os stick, not
used"). The other way round, a stick booted on a laptop that has an
installed copy uses the stick (sticks are looked for first), so a newer
stick can install over an older installation. To update an installed
laptop by hand, copy a new `atari-os/` folder onto its disk (it is a plain
FAT32 drive) or install again from a new stick (that erases `atari/` too:
copy anything you want to keep first).

### Next to Linux or Windows (dual boot)

The Atari can also live next to the system a laptop already has, and start
from that machine's boot menu. The release has
`atari-os-x86-<version>-files.zip` (the stick's files) and two scripts for
this. Nothing the scripts did not say they would change is changed: no
existing partition is formatted unless you ask, the default boot entry stays
what it was, and nothing is written before you confirm (the partition's
name and `yes` on Linux, the drive letter or `yes` on Windows). **Secure Boot must be off** (Limine and the Atari's kernel are
unsigned); both scripts check and stop if it is on. `--check` / `-Check`
shows what a script sees, `--dry-run` / `-WhatIf` the plan, and neither
changes anything.

**Linux, on a FAT32 partition** (any partition of 1.5 GiB or more that is
not mounted, not in `/etc/fstab`, not the EFI system partition, not swap,
LVM, RAID or encrypted; an empty FAT32 one, or with `--format` one you no
longer need):

```sh
sudo sh install-linux.sh --partition /dev/sdb3 atari-os-x86-v0.2.0-files.zip
sudo sh install-linux.sh --partition /dev/sdb3 --format atari-os-x86-v0.2.0-files.zip   # erases sdb3 first
```

The files are unpacked onto the partition (a normal FAT32 drive, so you can
add your Atari files from Linux too), and the GRUB menu gets an entry
"Atari" that starts the kernel from it (with `atari.media=<its serial>`).

**Linux, as an image file** (no partitioning at all): the `.img` (or
`.img.xz`) is copied as a file onto an ext4, btrfs or xfs filesystem, GRUB
loop-mounts it, and the Atari uses the FAT inside it read-write (your
library and settings live in that file):

```sh
sudo sh install-linux.sh --image /home/atari.img atari-os-x86-v0.2.0.img.xz
```

The path may only contain letters, digits and `. _ + - /`. Not on LVM or
encrypted volumes (GRUB and the Atari cannot open those). On btrfs the
script makes the file `chattr +C` (no copy-on-write, no compression) before
copying. The Atari mounts that filesystem read-write, so do not hibernate
Linux and then start the Atari.

How the GRUB entry is added: a file `custom.cfg` next to `grub.cfg`
(`/boot/grub/` or `/boot/grub2/`), which every `grub.cfg` made by
`update-grub`/`grub2-mkconfig` reads through `/etc/grub.d/41_custom`. The
script never edits `grub.cfg` or `/etc/default/grub` and never runs
`update-grub`; an existing `custom.cfg` that is not its own is left alone
(the script then prints the entry for you to add by hand, as it does when
the GRUB does not read `custom.cfg`). Ubuntu hides the menu when Linux is
the only system: hold **Shift** (BIOS) or press **Esc** (UEFI) right after
power-on to see it. Where `/etc/default/grub` has `GRUB_DEFAULT=saved`,
`sudo grub-reboot atari-os && sudo reboot` starts the Atari once (the entry
has the id `atari-os`). `sudo sh install-linux.sh --remove` deletes the
script's own `custom.cfg` and nothing else; the partition or image file
stays for you to delete.

**Windows (UEFI)**, from an administrator PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Check
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Zip atari-os-x86-v0.2.0-files.zip -ShrinkMB 2048
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Zip atari-os-x86-v0.2.0-files.zip -Partition E:
```

`-ShrinkMB n` (1536 to 32768) shrinks `C:` by n MB and makes a FAT32
partition "ATARI" in the freed space (BitLocker is fine); `-Partition E:`
uses an existing, empty FAT32 volume of 1.5 GB or more instead (the script
never formats an existing volume). The files are unpacked onto it and the
script adds **one firmware boot entry "Atari", appended at the end of the
boot list**: it shows in the machine's one-time boot menu (F12, F9, F8 or
Esc at power-on), and **the default stays Windows**; the script never
changes the default. `-BootNow` starts the Atari once at the next restart
and restarts. `-Menu` also puts "Atari" into the blue Windows Boot Manager
menu, for firmware that hides added entries (that menu then shows at every
start; some Windows versions refuse to start a non-Windows loader from it).
`-Remove` deletes the entries again (and puts the Boot Manager timeout
back); the partition and its files stay. The execution policy is not
changed: `-ExecutionPolicy Bypass` applies to that one run. With Windows'
Fast Startup on, Windows keeps the ATARI drive open over a shutdown: do not
change its files from both Windows and the Atari between two starts (or turn
Fast Startup off). **Tested on Windows 11 by: nobody yet.** BIOS-mode
Windows machines: use the stick.

## How it works

```
Firmware (BIOS or UEFI)
  └─ Limine                       one FAT32 stick, boots both ways
      └─ Linux (Alpine linux-lts) + initramfs
          └─ /init  (init/init.c, static)
              ├─ loads drivers by modalias (no udev)
              ├─ mounts the stick (or the disk it was installed on),
              │  attaches modloop (modules + firmware)
              └─ runs the emulator and keeps it running
                   video: DRM/KMS dumb buffers, CPU scaling, vsync page flips
                   audio: ALSA PCM directly
                   input: evdev directly, hotplug via inotify
```

The emulator core is [atari800](https://github.com/atari800/atari800) with a
new platform port (`emulator/kms/`, `emulator/sound_alsa.c`) that talks to the
hardware without SDL. The port is core-agnostic by design: Altirra's libretro
core is the planned second core.

## Stick layout

```
/EFI/BOOT/BOOTX64.EFI   /limine-bios.sys   /limine.conf
/atari-os/vmlinuz  initramfs  modloop.sqfs      ← the system (replace to update)
/atari/library  disks  carts  roms  config      ← yours
/atari/fujinet/fnconfig.ini  SD/                ← FujiNet's config and "SD card"
/atari/ash/ASH.CAR  ASH.XEX  ASH.ATR            ← ASH, the Atari shell
/atari/hdd/                                     ← ASH's h: drive
```

Drop your collection (any folders, any Atari files) in `library/`, and
(optionally) real Atari ROMs in `roms/`. Without ROMs the built-in Altirra
OS is used.

## The Library (F9)

F9 (or the first entry of the F8 menu) opens a browser over the sources,
cycled with **Tab**: **Stick** (the stick's whole `atari/` folder, opening
in `library/`: FujiNet's downloads are `Stick: /fujinet/SD`, and `disks/`,
`hdd/`, `ash/` are there too; `config/`, `ssh/` and `logs/` are not shown)
and every USB drive plugged in (init mounts them under
`/media/usb/<label>`; a border hint says so when one appears). RETURN on a
folder enters it, Backspace or `..` goes back up, Esc closes (from
`library/` or a source's root). RETURN on a file offers **Run**, **Run
with BASIC** (disk images: for BASIC-based disks such as magazine disks),
**Mount as D1:** to **D4:** (disk images, without a restart: swap disk 2
in mid-game; what is in D1: boots at the next cold start), or **Insert
cartridge**/**Insert tape** (the next cold start boots it), **Copy to
Library**, **Copy to FujiNet SD**, **Copy to...** a USB drive (keeping
the folder path; "Safe to unplug" once it is synced), **Favourite**, and
**Eject all & reboot** when something is mounted.
Under Source and Filter, every listing has a **Drives** line,
`[ Drives: D1 Game (Disk 1).atr  D2 Game (Disk 2).atr ]` (or `nothing
mounted`); RETURN on it lists D1:-D4:, the cartridge and the tape: RETURN
on one in use ejects it after a Yes (a disk without a restart; the
cartridge with one, as its program loses its ROM), and **Eject all &
reboot** goes back to the boot target.
`Stick: /` and `Stick: /library` list the last 8 programs run and the
favourites (`atari/config/library.cfg`). Multi-disk sets named
`... (Disk 1)...`, `..._d1...` or `... Side 1...` get disks 2-4 in D2:-D4:.
A filter entry narrows the listing to matching file names.

## The System menu (F8)

The laptop boots straight to BASIC `READY`. F8 (or a gamepad's Guide
button) opens a menu in the same style as the emulator's own, backed by
PID 1 over a Unix socket. Every setting lives in one place:

```
Library                         F9
Boot into: BASIC
Eject all, back to BASIC
Display...      Scaling, Smoothing, Scanlines, Borders, TV look,
                TV system, Brightness
Sound: 80%
Controls...     Arrows, Keyboard type, Gamepad 1 drives, Gamepad buttons
Network: <ssid>  Wi-Fi, IP, Remote access (SSH), SSH password
Help: keys and menus
Hardware status
Install onto this laptop's disk...   (stick + internal disk only)
Save logs to the stick
Restart emulator
Reboot laptop   (UEFI: straight back into Atari, or normal)
Shut down
Advanced: emulator menu...
Back to Atari                   Esc
```

- **Library**: the F9 Library
- **Boot into**: BASIC, No BASIC (the XL self test, as when holding
  OPTION), ASH (the Atari shell) or FujiNet CONFIG; choosing another one
  restarts the Atari into it (persists). This is what starts when nothing
  is mounted: **what is in D1: boots**. Every cold start (Advanced "Reboot",
  Shift+F5, the Library, ASH's `reboot`, F8) boots a disk in D1:, a
  program, a cartridge or a tape set to boot when there is one, with the
  ASH cartridge taken out and BASIC off for that boot (`Run with BASIC`
  keeps it on); `ASH.CAR` run by hand boots ASH whatever the target. With
  `boot=fujinet` a local image in D1:-D8: answers instead of FujiNet's
  drive (at standard SIO speed: no SIO patch with NetSIO)
- **Eject all, back to <target>**: empties D1:-D8:, takes out the
  cartridge (not ASH's own) and the tape, and cold starts into the target
- **Display...**: **Scaling** Integer (the largest whole factor, default),
  Fit (the full height at the Atari's shape) or Fill (the whole panel);
  **Smoothing** Sharp or Smooth for Fit/Fill (sharp-bilinear: pixels stay
  square blocks, only their edges are blended); **Scanlines** Off, Light,
  Medium, Strong (every second panel row of an Atari line at 75/50/25 %,
  from a 2x high picture up); **Borders** Full (384 columns), TV (336,
  default), Tight (320); **TV look** Crisp or NTSC TV (atari800's NTSC
  composite filter, NTSC only; the same setting as Advanced > Display
  Settings > "full NTSC filter", `ARTIFACT_NTSC=NTSC-FULL`); **TV system**
  and **Brightness** (below). Live and saved
  (`KMS_SCALE`, `KMS_SMOOTH`, `KMS_SCANLINES`, `KMS_BORDERS` in
  `atari800.cfg`); `-kms-debug` logs the render time per frame
- **Sound** (volume) and Display's **Brightness**: sliders, saved in the
  emulator config (`KMS_VOLUME`, `KMS_BRIGHTNESS`)
- **Controls...**: **Arrows** cursor keys or joystick 0 (the same switch
  as F11); **Keyboard type** PC (default: every key types what its cap
  says) or Atari (positional: the PC key in the place of an Atari key
  types that key, so Shift+2 is `"`, Shift+8 `@`, `-` is `<`, `=` `>`,
  `[` `-`, `]` `=`, `'` `+`, `\` `*`; the Help page shows the table);
  **Gamepad 1 drives** joystick 0 or 1 (swaps the first two gamepads;
  menus follow the first); **Gamepad buttons** (the mapping). Live and
  saved (`KMS_ARROWS_JOYSTICK`, `KMS_KEYBOARD=pc|atari`, `KMS_PAD_SWAP`)
- **Network...**: **Wi-Fi** (scan, pick a network, type the password;
  `wpa_supplicant` keeps the network in
  `atari/config/wpa_supplicant.conf`), the IP address, **Remote access
  (SSH)** on/off, **SSH password** (keys only until set)
- **Help: keys and menus**: the F1 page
- **TV system** (in Display): NTSC (60 Hz, the default: the Atari was an NTSC machine
  first) or PAL (50 Hz, one keypress away here), switched live and saved
  in the emulator config (`DEFAULT_TV_MODE`; a config that already says
  PAL stays PAL). When the panel's refresh is within
  0.75 Hz of the Atari's rate, the display paces the emulator (every Atari
  frame shown once: smooth scrolling); otherwise atari800's timer does.
  On a 60 Hz laptop panel, NTSC is the one that locks. The display mode is chosen at
  start-up (the mode closest to the Atari's rate at the panel's
  resolution; `KMS_MODE=WxH@Hz` in `atari800.cfg` or `-kms-mode` forces
  one), so a panel with a 50 Hz mode picks it up for PAL after "Restart
  emulator".
- **Hardware status** page: machine, CPU, RAM, CPU use (emulator,
  FujiNet), temperature and package power, video driver and mode, display
  options, TV system and pacing, sound card, Wi-Fi, IP, battery, boot
  medium, boot target, what the last cold start booted (and whether BASIC
  was on), kernel
- **Install onto this laptop's disk...** (only when running from a stick
  and an internal disk is there; see "Install onto the laptop's disk")
- Save logs to the stick, restart emulator
- **Reboot laptop**; on UEFI machines a popup asks **Straight back into
  Atari** (`BootNext`, a one-shot request for the next start only; the
  machine's boot order is not touched) or **Normal reboot** (the machine's
  own boot order: the other OS on a dual-boot laptop)
- **Shut down** (besides the power button, the only way to power off: F9
  no longer quits)
- **Advanced: emulator menu...**: atari800's own menu, with everything it
  has (save states, machine type, cartridge types, disk and tape
  management, recording). Its "System menu" entry comes back here, Esc
  goes back to the Atari; it also has "Library" and "Shut down the
  laptop"

Boot shows an Atari-style splash with progress lines; the first boots show
a "F1 = Help   F8 = System   F9 = Library" hint under the picture until
one of the three is opened.

### Help (F1)

F1 shows every key on one page (any key turns to the next, Esc closes):
the function keys, the Atari keys on the PC keyboard, the joystick keys,
the current keyboard type, the gamepad; a second page has the Library and
menu keys and how to power off; with the Atari keyboard type a third page
has the positional table. The same pages are F8 > "Help: keys and menus".

### Remote access

SSH is dropbear, root, off by default. Keys only until a password is set
in the System menu (needed when the stick, and the key on it, is in the
laptop). On first use init generates a key pair and writes the private key
to the stick: `atari/ssh/id_ed25519`; whoever holds the stick may log in.
Images built here also authorise the builder's key from `build/ssh/`.
From another machine:

```sh
ssh -i /media/$USER/ATARI/atari/ssh/id_ed25519 root@<ip shown in the menu>
```

Settings live in `atari/config/system.conf` (`boot=basic|nobasic|ash|fujinet`,
`ssh=`; an older `fujinet=1` is read as `boot=fujinet`).

## ASH

[ASH](https://github.com/carlosbravoa/atari800-atari-shell) is a Unix-style
shell for the Atari that replaces the DOS menu: `ls`, `cd`, `cat`, `cp`,
`mv`, `rm`, `run`, `mount`, an editor, `help` for the rest. It is a
submodule (`third_party/atari800-atari-shell`); the stick carries its
prebuilt cartridge and program in `atari/ash/`. **Boot into: ASH** in the
F8 menu inserts `ASH.CAR` and restarts; RESET in a program started from
ASH returns to the shell, `basic` leaves it, `reboot` cold-starts into it
again.

ASH was written for the Tang Nano 20K FPGA Atari, whose SD card it reaches
through an SIO file device ($72). The emulator provides that device here,
backed by the stick:

- `h:` is the folder `atari/hdd/`: flat, 8.3 names (`ls h:`, `cat h:readme.txt`).
- `sd:` is the stick and the USB drives: `sd:/atari/...` is the stick's
  `atari/` folder, `sd:/usb/<drive>/...` a USB drive. `mount d2: sd:/...`
  mounts a disk image (a `.xex` on D1: is run), `umount d2:`, `mount`
  alone lists what is mounted, `cart sd:/...` inserts a cartridge and
  restarts.
- Writing to `sd:` is off until `sd rw` (until the next cold start);
  `atari/ash/`, `atari/config/`, mounted disks and the inserted cartridge
  are never written.

The device rides atari800's SIO patch, so it is absent in FujiNet mode
(NetSIO); ASH then shows no `h:`.

## FujiNet

[FujiNet-PC](https://github.com/FujiNetWIFI/fujinet-firmware) runs next to
the emulator, connected over NetSIO. With FujiNet boot on, the Atari boots
into FujiNet CONFIG with the usual TNFS hosts, and `fujinet/SD/` on the
stick is its SD card. Wired Ethernet gets an address by DHCP; Wi-Fi is set
up from the System menu. FujiNet's web UI is reachable on the LAN at
`http://<laptop>:8000/`. Boot with `atari.nofujinet=1` to not start it.

FujiNet uses the laptop's network: it is online whenever the laptop has an
address, over Wi-Fi or Ethernet. CONFIG's Wi-Fi screens scan for and join
networks with the laptop's Wi-Fi, the same as the F8 System menu does.

**The SD card** is the folder `atari/fujinet/SD/` on the stick; CONFIG
lists it as the host `SD` (it starts with a `README.TXT`). Files get onto
it in several ways:

- the Library (F9): pick a file on the stick or a USB drive, **Copy to
  FujiNet SD** (it is `Stick: /fujinet/SD` in the Library);
- copy them into `atari/fujinet/SD/` with the stick in another computer,
  or over SSH (there is no scp/sftp on the appliance; stream it:
  `ssh root@<ip> 'cat > /media/atari/fujinet/SD/GAME.ATR' < GAME.ATR`);
- CONFIG's own copy between hosts (a TNFS host to SD). TNFS hosts are
  read-only: copies go from them, never to them.

Disks mounted on D1:-D8: in CONFIG are FujiNet's own slots (kept in
`atari/fujinet/fnconfig.ini`), separate from the Library's "Mount as D1:",
which mounts in the emulator.

FujiNet-PC is built from source for musl with small patches
(`fujinet/patches/`: musl fixes, the network bridge above) and with
`DEBUG_NO_REBOOT`, so it survives the Atari's cold resets instead of
restarting itself.

## Keys

| Key | Atari |
|---|---|
| F1 | Help: every key (the stock atari800 menu is F8 > Advanced) |
| F2 / F3 / F4 | OPTION / SELECT / START |
| F5, Shift+F5 | warm / cold reset |
| F6, F7 | HELP, BREAK |
| F8 | System menu (Library, boot target, display, sound, controls, network, power) |
| F9 | Library: browse, run, mount and copy Atari files (the stick, FujiNet SD on it, USB drives) |
| F10, Shift+F10 | screenshot, interlaced screenshot (PCX, saved next to the config) |
| F11 | toggle arrow keys between cursor keys (default) and joystick 0 |
| F12 | turbo on/off |
| ` or Win | Atari (inverse video) key |
| Home, Ins, Del | CLEAR, insert, delete (Shift: line) |
| Caps Lock | CAPS |
| Shift+arrow | cursor keys too (no longer `- = + *`) |
| Arrows + left Alt | joystick 0 after F11 (numpad 8/2/4/6 + 0 always) |

With the default keyboard type (PC) every key types what its cap says;
F8 > Controls switches to the Atari's own layout (positional, see the
System menu). USB gamepads work out of the box: D-pad/left stick =
joystick, A/triggers = fire, Start/Select/X = START/SELECT/OPTION, B = Esc
in menus, Guide = System menu.

## Building

Everything builds inside an Alpine Linux root under `build/alpine/`, created
without root through user namespaces. Needs: `unshare` (util-linux), `curl`,
`gcc` on the host; QEMU and OVMF to test.

```sh
scripts/alpine-env.sh bootstrap                  # once
scripts/alpine-env.sh run scripts/build-emulator.sh
scripts/alpine-env.sh run scripts/build-fujinet.sh   # optional, needs network
scripts/alpine-env.sh run scripts/mkimage.sh     # -> build/atari-os-x86-<version>.img
scripts/run-qemu.sh bios                         # or: uefi
```

The build scripts find the project from their own location and refuse to
run outside Alpine; `scripts/alpine-env.sh pkgs` prints the package list,
which is all a plain `alpine` container needs (that is what CI does).

The version is `git describe --tags --always` plus `-dirty` for uncommitted
changes in this repository (the build's own patches inside the submodules
do not count); `ATARI_OS_VERSION` overrides it. It is in the image name,
in `/atari-os/VERSION` on the stick and `/etc/atari-os-release` in the
initramfs, on the boot splash, on the System menu's hardware page and in
init's `status` reply. `build/atari-os.img` is a hard link to the newest
image, for the local scripts; older `build/atari-os-x86-*.img` are removed
(`KEEP_OLD_IMAGES=1` keeps them). `DEV_SSH_KEY=0` leaves the developer key
out (release images).

Write the image with `scripts/flash.sh build/atari-os.img` (see Install).
Secure Boot must be disabled on UEFI machines.

### Releases and CI

`.github/workflows/build.yml` runs on every push and pull request: the image
in an `alpine:3.24` container (uploaded as the `image` artifact, `.img.xz`
plus `SHA256SUMS`), the emulator against Ubuntu's libraries with
`-Werror` (the C89 check) plus a static `init`, and a smoke boot of the
image in QEMU, BIOS and UEFI, that fails unless the emulator starts
(screenshots and serial logs in the `smoke` artifact). Pushing a `v*` tag
runs `release.yml`: the same workflow, then a GitHub release with the
image, the dual-boot files zip, `SHA256SUMS`, `flash.sh`,
`install-linux.sh` and `install-windows.ps1`, its text from
`.github/release-template.md` above the generated notes. `CHANGELOG.md`
lists the changes. `tests/flash-test.sh` tests `flash.sh` without touching
any disk (`--to-file` and a faked disk list).

**Ventoy:** copy the image (`atari-os-x86-<version>.img`, or
`build/atari-os.img`) onto a Ventoy stick as a file and pick it
from the Ventoy menu (Legacy or UEFI). After the kernel starts, init looks
for an `atari-os*.img` file on every partition (FAT, exFAT, NTFS),
loop-mounts it and uses the FAT inside as the stick, so config and FujiNet
files persist inside the image. `scripts/mk-ventoy-test.sh` builds a
QEMU disk that exercises this path.

Pick the *debug* entry in the boot menu to get a root shell on the serial
console and verbose kernel messages.

### Headless testing

`scripts/qemu-test.sh` drives QEMU without a display: `start [bios|uefi]`,
`wait [seconds]` (until the emulator runs), `key <name>` (QEMU sendkey
names), `shot <png>`, `serial "<cmd>"` (runs a command on the debug shell),
`log`, `mon "<cmd>"` (QEMU monitor), `stop`. `QEMU_IMG` picks another image,
`OVMF_CODE` the UEFI firmware. Build a debug image with
`EXTRA_CMDLINE="console=ttyS0,115200 atari.debug=1"` for `serial` to work,
or turn a release image into one with `qemu-test.sh debug-image <img>`
(edits its `limine.conf`; needs mtools).
`type "<text>"` types a string, one `sendkey` per character on a US layout
(`\n` is RETURN; `QEMU_TYPE_DELAY`, default 0.15 s, between keys), which is
how ASH is driven headless, e.g. `scripts/qemu-test.sh type 'ls sd:/\n'`.
Menu positions for `key` recipes: F8 then `down` 12 times and `ret` is
**Shut down** (13 times when "Install onto this laptop's disk..." is
shown, i.e. with an internal disk attached), 13 times is "Advanced:
emulator menu...", whose "System menu" entry is 19 `down`s from its top.
`QEMU_EXTRA` adds QEMU arguments; `scripts/mk-usb-test.sh` builds
`build/usb-test.img`, a USB drive with Atari files for the Library:

```sh
QEMU_EXTRA="-drive if=none,id=usb2,format=raw,file=build/usb-test.img \
  -device usb-storage,bus=xhci.0,drive=usb2,removable=on" scripts/qemu-test.sh start
```

The installer needs internal disks that keep what is written to them:
`scripts/mk-install-test.sh [file] [size]` makes a blank sparse
`build/install-test.img` (8G), and `QEMU_DISK="file[:ide|virtio|nvme] ..."`
attaches each as a non-removable disk with `snapshot=off` (everything else
stays `-snapshot`). `QEMU_IMG_AS=ide|virtio|nvme` boots the main image as an
internal disk instead of a USB stick, and `QEMU_VARS=<file>` gives OVMF a
persistent variable store (copied from `OVMF_VARS_4M.fd` when missing), so
the installer's boot entry survives into the next run. In BIOS mode the
main image gets `bootindex=0` (SeaBIOS only tries one hard disk); UEFI runs
get none, so OVMF keeps its own `BootOrder`.

```sh
scripts/mk-install-test.sh
QEMU_VARS=build/qemu-test/vars.fd QEMU_DISK=build/install-test.img scripts/qemu-test.sh start uefi
#   F8 > Install onto this laptop's disk... > ERASE, then RETURN (QEMU exits on reboot)
QEMU_VARS=build/qemu-test/vars.fd QEMU_IMG=build/install-test.img QEMU_IMG_AS=ide scripts/qemu-test.sh start uefi
```

The dual-boot paths have their own test disks. `QEMU_KERNEL=1` boots
without any stick: QEMU loads `build/stick/atari-os/vmlinuz` and
`initramfs` itself (`-kernel`/`-initrd`) with the first `cmdline:` of
`build/stick/limine.conf`, the serial shell and `$QEMU_APPEND`, the way a
host's boot loader starts us. `scripts/mk-hostfs-test.sh` makes
`build/hostfs-test.img`, a 4 GiB ext4 filesystem (no partition table,
`mkfs.ext4 -d`, no root needed) holding the image as
`/atari/atari-os-x86.img`; `scripts/mk-grub-test.sh partition|image` (in
the Alpine env, after `apk add grub grub-bios e2fsprogs`) makes
`build/grub-test.img`, a disk with a BIOS GRUB, a `grub.cfg` in the shape
`grub-mkconfig` writes and the `custom.cfg` that `install-linux.sh` writes
(its own functions), booting the FAT32 partition 2 or the image file on the
ext4 partition 1 (`GRUB_EFI=1` adds an x86_64-efi GRUB on partition 2 for
`start uefi`):

```sh
scripts/mk-hostfs-test.sh
QEMU_KERNEL=1 QEMU_DISK=build/hostfs-test.img \
  QEMU_APPEND="atari.image=a7a12000-0000-4000-8000-000000000001:/atari/atari-os-x86.img" scripts/qemu-test.sh start
scripts/alpine-env.sh run scripts/mk-grub-test.sh image
QEMU_IMG=build/grub-test.img QEMU_IMG_AS=ide scripts/qemu-test.sh start bios
```

The socket commands behind it are `disks` (`disk <name> <bytes> <model>`
lines) and `install <name>` (streams `step <text>` and `progress <pct>`
lines, then `OK` or `ERR <text>`); `status` has `media
stick|image-file|disk|none` and `installable <n>`. Every step is logged as
`install: ...` in dmesg.

Kernel command-line knobs for debugging: `atari.perf=1` (every 10 s, CPU
use of atari800, fujinet, wpa_supplicant, dropbear and init, package power
from RAPL, CPU temperature and clock to `atari/logs/perf.log`; the socket
command `perf on|off` does the same at run time), `atari.debug=1` (serial shell,
`-evdev-debug -kms-debug` on the emulator, emulator exit restarts instead of
powering off), `atari.noemu=1`, `atari.emudelay=N`, `atari.nomedia=1`,
`atari.media=XXXX-XXXX` (use the FAT32 volume with that serial as the
stick; the installers set it), `atari.image=<UUID>:<path>` (loopback mode:
mount the filesystem with that UUID read-write on `/.host` and use the image
file at `<path>` on it as the stick; `install-linux.sh --image` sets it, and
`status` then has a line `image <device> <path>`),
`atari.hwsim=1` (two virtual Wi-Fi radios via `mac80211_hwsim`, so the
Wi-Fi menu can be exercised in QEMU against an access point started on
`wlan1` from the debug shell). `QEMU_NONET=1` starts QEMU without a
network card.

Two things learned the hard way: fbcon's deferred takeover of a DRM fbdev
commits over the emulator's scanout, hence `drm_kms_helper.fbdev_emulation=0`
on the command line; and QEMU under TCG delivers a USB key's press and
release in one evdev read, so the input layer keeps a tap alive for one frame.

## Status

Runs on real laptops. Verified on an ASUS ZenBook UX425EA (Tiger Lake):
boot to emulator in 5.6 s, i915 at 1920x1080 with a 4x picture, SOF audio,
Wi-Fi from the System menu, SSH, volume/brightness keys, clock sync.
QEMU-verified: BIOS and UEFI, BASIC and FujiNet CONFIG boots, TNFS over
the internet, DHCP, power button. Next: the media browser, lid switch,
the Altirra core, i686 netbooks; see KNOWN-ISSUES.md.

## Licence

GPL-2.0-or-later. atari800 is GPL-2.0-or-later; Limine is BSD-2-Clause;
firmware files carry their own licences (see `linux-firmware`).
