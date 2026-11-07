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

## How it works

```
Firmware (BIOS or UEFI)
  └─ Limine                       one FAT32 stick, boots both ways
      └─ Linux (Alpine linux-lts) + initramfs
          └─ /init  (init/init.c, static)
              ├─ loads drivers by modalias (no udev)
              ├─ mounts the stick, attaches modloop (modules + firmware)
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

F9 (or the first entry of the F1 menu) opens a browser over three kinds of
source, cycled with **Tab**: the `library/` folder on the stick, every USB
drive plugged in (init mounts them under `/media/usb/<label>`; a border hint
says so when one appears), and FujiNet's `fujinet/SD/` folder. RETURN on a
folder enters it, Backspace or Esc goes back up; RETURN on a file offers
**Run**, **Mount as D1:** (or insert the cartridge/tape), **Copy to** any
other source (keeping the folder path; "Safe to unplug" once it is synced),
and **Favourite**. The Library root lists the last 8 programs run and the
favourites (`atari/config/library.cfg`). Multi-disk sets named
`... (Disk 1)...`, `..._d1...` or `... Side 1...` get disks 2-4 in D2:-D4:.
A filter entry narrows the listing to matching file names.

## The System menu (F8)

The laptop boots straight to BASIC `READY`. F8 opens a menu in the same
style as the emulator's own, backed by PID 1 over a Unix socket:

- **Boot into**: BASIC, ASH (the Atari shell) or FujiNet CONFIG; choosing
  another one restarts the Atari into it (persists)
- **Volume** and **Brightness** sliders (persist in the emulator config)
- **Wi-Fi**: scan, pick a network, type the password; `wpa_supplicant`
  keeps the network in `atari/config/wpa_supplicant.conf`
- **Remote access (SSH)** on/off, **SSH password** (keys only until set)
- **TV system**: NTSC (60 Hz, the default: the Atari was an NTSC machine
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
  FujiNet), temperature and package power, video driver and mode, TV
  system and pacing, sound card, Wi-Fi, IP, battery, boot medium, boot
  target, kernel
- **Reboot straight back into Atari** (UEFI `BootNext`, a one-shot request
  for the next start only; the machine's boot order is never touched)
- Save logs to the stick, restart emulator, reboot, **shut down** (besides
  the power button, the only way to power off: F9 no longer quits)

Boot shows an Atari-style splash with progress lines; the first boots show
an "F8 = System menu" hint under the picture until the menu is opened once.
The emulator's own F1 menu has "System" and "Shut down" entries.

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

Settings live in `atari/config/system.conf` (`boot=basic|ash|fujinet`,
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

- the Library (F9): pick a file on the stick or a USB drive, **Copy to**
  FujiNet SD (the tab is titled "FujiNet SD (host SD in CONFIG)");
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
| F1 | menu |
| F2 / F3 / F4 | OPTION / SELECT / START |
| F5, Shift+F5 | warm / cold reset |
| F6, F7 | HELP, BREAK |
| F8 | System menu (FujiNet, volume, brightness, Wi-Fi, SSH, power) |
| F9 | Library: browse, run and copy Atari files (stick, USB drives, FujiNet SD) |
| F10 | screenshot (PCX, saved next to the config) |
| F11 | toggle arrow keys between cursor keys (default) and joystick 0 |
| Arrows + left Alt | joystick 0 after F11 (numpad 8/2/4/6 + 0 always) |
| ` or Win | Atari (inverse video) key |

USB gamepads work out of the box; Start/Select/X map to the console keys and
the guide button opens the menu.

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
image, `SHA256SUMS` and `flash.sh`, its text from
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
`QEMU_EXTRA` adds QEMU arguments; `scripts/mk-usb-test.sh` builds
`build/usb-test.img`, a USB drive with Atari files for the Library:

```sh
QEMU_EXTRA="-drive if=none,id=usb2,format=raw,file=build/usb-test.img \
  -device usb-storage,bus=xhci.0,drive=usb2,removable=on" scripts/qemu-test.sh start
```

Kernel command-line knobs for debugging: `atari.perf=1` (every 10 s, CPU
use of atari800, fujinet, wpa_supplicant, dropbear and init, package power
from RAPL, CPU temperature and clock to `atari/logs/perf.log`; the socket
command `perf on|off` does the same at run time), `atari.debug=1` (serial shell,
`-evdev-debug -kms-debug` on the emulator, emulator exit restarts instead of
powering off), `atari.noemu=1`, `atari.emudelay=N`, `atari.nomedia=1`,
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
