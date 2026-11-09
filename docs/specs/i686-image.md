# Spec: a 32-bit (i686) image for the machines before x86_64

Status: Carlos, 2026-11-09: "I would like to add also as another
experiment, a 32 bits image for even older machines". Design: Claude
(Fable); implementer: an Opus agent in its own worktree; verification:
Claude in QEMU, Carlos on hardware later. Rules as in `docs/specs/menus.md`
§0 and `docs/specs/library.md` §3/§7 (`-Werror`, C90, nothing committed by
the agent). The x86_64 build must come out unchanged in behaviour: every
new thing is behind an architecture switch whose default is x86_64.

## Target

Laptops with a 32-bit-only CPU: Core Duo and Core Solo (2006), Pentium M,
Celeron M, Atom N270/N280 netbooks (2008-2010). Core 2 Duo and Atom N450+
are 64-bit and already boot the x86_64 image. These machines have BIOS
firmware (no UEFI), 512 MB to 2 GB of RAM, a 1024x600 or 1280x800 panel
on an Intel GMA 950/X3100/GMA 3150 (i915 driver), Intel or Atheros or
Broadcom Wi-Fi, USB 2.0. Some Pentium M (Banias) have no PAE; the Alpine
x86 `linux-lts` kernel's config decides whether they boot: report what
`CONFIG_X86_PAE`/`CONFIG_HIGHMEM` say (the config is in the kernel package
or `/proc/config.gz` in QEMU).

## Pieces

Alpine 3.24 x86 has everything: minirootfs
`alpine-minirootfs-3.24.4-x86.tar.gz`, `linux-lts 6.18.62-r0` (the same
version as x86_64), `limine 12.9.6`, `limine-efi-x86`, `libdrm`,
`alsa-lib`, `dropbear`, `mtools`, `squashfs-tools`. `scripts/alpine-env.sh`
already has `ALPINE_ARCH` (default x86_64).

1. **Build environment**: `ALPINE_ARCH=x86 scripts/alpine-env.sh bootstrap`
   must produce a second environment next to the x86_64 one, not replace
   it (an arch-specific directory; read how the script names its root
   today and make the name include the arch when it is not x86_64, so
   the existing x86_64 path stays exactly where it is). Running x86
   binaries on the x86_64 host works as it does for x86_64 (same
   mechanism, bubblewrap/chroot/unshare, whatever the script uses); set
   the personality with `setarch i686` (or `linux32`) around the run so
   `uname -m` says `i686` and configure scripts pick the right triplet.
   If `setarch` is unavailable inside, pass `--host=i686-alpine-linux-musl`
   to the configure scripts instead.
2. **`scripts/mkimage.sh`** takes `ARCH` (`x86_64` default, `x86`): kernel
   package and modules from the arch environment, `EFI/BOOT/BOOTIA32.EFI`
   from `limine-efi-x86` for x86 (also keep `BOOTX64.EFI` only if Limine
   can start a 32-bit kernel from 64-bit UEFI: test it, see Verification;
   otherwise leave it out and say so in the README), BIOS loader as today.
   Output names: `build/atari-os-x86-<version>-i686.img`,
   `...-i686-files.zip`, `build/atari-os-i686.img` as the unversioned copy
   (x86_64 keeps `build/atari-os.img`). The version string written to
   `/atari-os/VERSION` and `/etc/atari-os-release` is `<version>-i686`
   so the Hardware page and `status` say which image it is. The cleanup
   that deletes `build/atari-os-x86-*.img` at the start must only delete
   its own architecture's images (`-i686.img` ones for x86, the others
   for x86_64). The `docs/specs/zenbook-modules.txt` safety check runs
   as it is if the module names exist in the x86 tree; if some do not
   (report which), skip only those for x86 and keep the check.
   Firmware packages: the same list; `linux-firmware-*` are
   architecture-independent (`noarch`) in Alpine, check they install.
3. **The programs**: `init/init.c` (static), the atari800 KMS port
   (`scripts/build-emulator.sh`), FujiNet-PC (`scripts/build-fujinet.sh`
   or wherever it is built), and whatever else `mkimage.sh` copies into
   the initramfs, built inside the x86 environment with `-Werror`. Fix
   32-bit warnings properly: `size_t`/`ssize_t` with `%zu`/`%zd`, 64-bit
   `time_t` and `off_t` on musl i686 (`_FILE_OFFSET_BITS=64` is the musl
   default; cast to `long long` for printing), pointer/int casts. Keep
   every fix valid on x86_64 too (the host `-Werror` build and the x86_64
   image must still build; run both). ASH and the Atari-side files are
   not architecture-dependent.
4. **QEMU**: `scripts/qemu-test.sh` gets `QEMU_SYSTEM` (default
   `qemu-system-x86_64`) and `QEMU_CPU` environment variables, so the
   32-bit image can be booted with `QEMU_SYSTEM=qemu-system-i386
   QEMU_CPU=n270` (an Atom N270: 32-bit, no SSE4, PAE) and with
   `QEMU_CPU=pentium3` (no PAE, no SSE2). `-m 512` for one run. The
   `uefi32` firmware (Arch Linux `edk2-ovmf` IA32 files under
   `build/ovmf32/`, see `docs/specs/firmware-2.md` B.3; if the other agent
   has not added them yet, add them the same way) tests the UEFI case,
   which real 32-bit laptops will not have but 32-bit-UEFI Atom tablets
   do.
5. **CI**: do not change `.github/workflows/*`. Say in the report what a
   matrix entry for x86 would need (which steps are arch-aware now).

## Docs

README: a short "32-bit machines" section (which CPUs, which image, BIOS
only unless the UEFI test says otherwise, RAM, that it is untested on
hardware so far, and how to build it: `ARCH=x86 scripts/alpine-env.sh run
scripts/mkimage.sh` or whatever the final form is). CHANGELOG under
Unreleased. KNOWN-ISSUES: "32-bit image: untested on real hardware; PAE
note". `.github/release-template.md`: one line for the i686 image under
Install (the release workflow does not build it yet).

## Verification

1. Host `-Werror` build and the x86_64 image still build and boot in
   QEMU (BIOS) to the Atari: unchanged.
2. The i686 image boots in `qemu-system-i386 -cpu n270 -m 512` (BIOS) to
   the Atari prompt: screenshot. `uname -m` says `i686`, the Hardware
   page shows `Version: ...-i686`, `pacing display` or `timer` in the
   log, sound device found (QEMU HDA), `lsmod` sane, FujiNet-PC running
   (`ps`), the Library opens (F9), a key typed in BASIC appears.
3. `-cpu pentium3`: boots, or report the exact failure (a PAE kernel
   would stop very early; that decides the KNOWN-ISSUES note).
4. `uefi32` with the i686 image: report. `uefi` (64-bit OVMF) with the
   i686 image: report (decides whether BOOTX64.EFI stays on it).
5. Sizes: `ls -l` of both images' `atari-os/` files; the i686 modloop
   should be close to the x86_64 one.
6. Delete the debug copies and anything else large you created in
   `build/` except the two i686 images and the x86 build environment.

## Report

Files changed; the exact commands to build the i686 image; the
screenshots' paths; QEMU results per 2-4; the PAE finding; the 32-bit
fixes in C code with the warnings they fixed (so the review can judge
them); and anything in this spec that turned out wrong.
