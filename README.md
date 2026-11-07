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
/atari/library  disks  carts  roms  config      ← yours
/atari/fujinet/fnconfig.ini  SD/                ← FujiNet's config and "SD card"
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

- **FujiNet boot** on/off: restarts the Atari into FujiNet CONFIG (persists)
- **Volume** and **Brightness** sliders (persist in the emulator config)
- **Wi-Fi**: scan, pick a network, type the password; `wpa_supplicant`
  keeps the network in `atari/config/wpa_supplicant.conf`
- **Remote access (SSH)** on/off, **SSH password** (keys only until set)
- **Hardware status** page: machine, CPU, RAM, video driver and mode,
  sound card, Wi-Fi, IP, battery, boot medium, kernel
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

Settings live in `atari/config/system.conf` (`fujinet=`, `ssh=`).

## FujiNet

[FujiNet-PC](https://github.com/FujiNetWIFI/fujinet-firmware) runs next to
the emulator, connected over NetSIO. With FujiNet boot on, the Atari boots
into FujiNet CONFIG with the usual TNFS hosts, and `fujinet/SD/` on the
stick is its SD card. Wired Ethernet gets an address by DHCP; Wi-Fi is set
up from the System menu. FujiNet's web UI is reachable on the LAN at
`http://<laptop>:8000/`. Boot with `atari.nofujinet=1` to not start it.

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
command on the debug shell), `log`, `mon "<cmd>"` (QEMU monitor), `stop`.
Build a debug image with
`EXTRA_CMDLINE="console=ttyS0,115200 atari.debug=1"` for `serial` to work.
`QEMU_EXTRA` adds QEMU arguments; `scripts/mk-usb-test.sh` builds
`build/usb-test.img`, a USB drive with Atari files for the Library:

```sh
QEMU_EXTRA="-drive if=none,id=usb2,format=raw,file=build/usb-test.img \
  -device usb-storage,bus=xhci.0,drive=usb2,removable=on" scripts/qemu-test.sh start
```

Kernel command-line knobs for debugging: `atari.debug=1` (serial shell,
`-evdev-debug -kms-debug` on the emulator, emulator exit restarts instead of
powering off), `atari.noemu=1`, `atari.emudelay=N`, `atari.nomedia=1`.

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
