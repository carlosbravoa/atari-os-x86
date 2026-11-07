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
/atari/disks  carts  roms  config               ← yours
/atari/fujinet/fnconfig.ini  SD/                ← FujiNet's config and "SD card"
```

Drop `.ATR` disk images in `disks/`, cartridges in `carts/`, and (optionally)
real Atari ROMs in `roms/`. Without ROMs the built-in Altirra OS is used.

## FujiNet

[FujiNet-PC](https://github.com/FujiNetWIFI/fujinet-firmware) runs next to
the emulator, connected over NetSIO, so the Atari boots into FujiNet CONFIG
with the usual TNFS hosts, and `fujinet/SD/` on the stick is its SD card.
Wired Ethernet gets an address by DHCP; Wi-Fi is the next phase. FujiNet's
web UI is reachable on the LAN at `http://<laptop>:8000/`. Boot with
`atari.nofujinet=1` for a plain Atari.

FujiNet-PC is built from source for musl with two small patches
(`fujinet/patches/`) and with `DEBUG_NO_REBOOT`, so it survives the Atari's
cold resets instead of restarting itself.

## Keys

| Key | Atari |
|---|---|
| F1 | menu |
| F2 / F3 / F4 | OPTION / SELECT / START |
| F5, Shift+F5 | warm / cold reset |
| F6, F7 | HELP, BREAK |
| F9 | quit (powers the laptop off) |
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
scripts/alpine-env.sh run scripts/mkimage.sh     # -> build/atari-os.img
scripts/run-qemu.sh bios                         # or: uefi
```

Write `build/atari-os.img` to a USB stick with `dd` (or any imaging tool).
Secure Boot must be disabled on UEFI machines.

**Ventoy:** copy `atari-os.img` onto a Ventoy stick as a file and pick it
from the Ventoy menu (Legacy or UEFI). After the kernel starts, init looks
for an `atari-os*.img` file on every partition (FAT, exFAT, NTFS),
loop-mounts it and uses the FAT inside as the stick, so config and FujiNet
files persist inside the image. `scripts/mk-ventoy-test.sh` builds a
QEMU disk that exercises this path.

Pick the *debug* entry in the boot menu to get a root shell on the serial
console and verbose kernel messages.

### Headless testing

`scripts/qemu-test.sh` drives QEMU without a display: `start [bios|uefi]`,
`key <name>` (QEMU sendkey names), `shot <png>`, `serial "<cmd>"` (runs a
command on the debug shell), `log`, `stop`. Build a debug image with
`EXTRA_CMDLINE="console=ttyS0,115200 atari.debug=1"` for `serial` to work.

Kernel command-line knobs for debugging: `atari.debug=1` (serial shell,
`-evdev-debug -kms-debug` on the emulator, emulator exit restarts instead of
powering off), `atari.noemu=1`, `atari.emudelay=N`, `atari.nomedia=1`.

Two things learned the hard way: fbcon's deferred takeover of a DRM fbdev
commits over the emulator's scanout, hence `drm_kms_helper.fbdev_emulation=0`
on the command line; and QEMU under TCG delivers a USB key's press and
release in one evdev read, so the input layer keeps a tap alive for one frame.

## Status

Phases 1 and 2 done in QEMU (BIOS and UEFI): boots into FujiNet CONFIG
(or BASIC without FujiNet), browses TNFS hosts over the internet; keyboard,
menu, sound, DHCP and power button work. Next, in order: real-hardware
testing, Wi-Fi (set up from CONFIG), the media browser and laptop keys (lid,
battery, backlight), the Altirra core, i686 netbooks.

## Licence

GPL-2.0-or-later. atari800 is GPL-2.0-or-later; Limine is BSD-2-Clause;
firmware files carry their own licences (see `linux-firmware`).
