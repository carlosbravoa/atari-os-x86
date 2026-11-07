# Spec: release pipeline and installer

Status: agreed 2026-11-07 ("release pipeline properly (not my script since
it could destroy things in other environments)"). Design: Claude (Fable);
implementer: an Opus agent; review and commits: Claude. Rules as in
`docs/specs/library.md` §3/§7 (don't commit; report; C89 for C code;
POSIX sh for scripts unless a script is explicitly bash).

## 1. Goals

A newcomer goes to the GitHub Releases page, downloads one file, writes it
to a USB stick **without any chance of wiping the wrong disk**, boots. Every
push is built and smoke-booted in CI so a broken image never becomes a
release. Every image knows its own version.

## 2. Versioning

- Version string = `git describe --tags --always --dirty` (e.g. `v0.3.0`,
  `v0.3.0-12-gabc1234`, `abc1234-dirty`), computed by `scripts/mkimage.sh`
  (override with `ATARI_OS_VERSION`).
- Baked into: the initramfs as `/etc/atari-os-release` (one line) and the
  stick as `/atari-os/VERSION`. The image file name is
  `atari-os-x86-<version>.img` (`mkimage.sh` also keeps the stable
  `build/atari-os.img` name for the local scripts; make it a hard link or
  copy, say which).
- Shown: boot splash second line (`atari-os-x86 <version>`), the hardware
  page (`Version: <version>`), and the control socket `status` reply
  (`version <version>`). init reads `/etc/atari-os-release`.

## 3. Build scripts run anywhere Alpine runs

Today `scripts/build-emulator.sh`, `build-fujinet.sh`, `mkimage.sh` and
`mk-ventoy-test.sh` hard-code `/work`. Change them to derive the project
root from their own location (`PROJECT=$(cd "$(dirname "$0")/.." && pwd)`),
so they run unchanged inside `scripts/alpine-env.sh run …` (where that is
`/work`) **and** in a plain Alpine container in CI. `scripts/alpine-env.sh`
gets a `pkgs` subcommand that prints `BUILD_PKGS` on one line, so CI
installs exactly the local list; and a check at the top of each build
script: if `/etc/alpine-release` is missing, die with "run me through
scripts/alpine-env.sh run". Verify the local flow still works end to end
(bootstrap is not needed; `run scripts/build-emulator.sh`, `run
scripts/mkimage.sh`, `qemu-test.sh start`).

## 4. GitHub Actions

`.github/workflows/build.yml`, on `push` and `pull_request`:

- job **image**: `runs-on: ubuntu-latest`, `container: alpine:3.24`
  (same branch as `ALPINE_BRANCH`; read it from `scripts/alpine-env.sh`
  or keep the two in sync with a comment). Steps: `apk add git $(pkgs)`
  (checkout needs git first: install `git` before `actions/checkout@v4`
  with `submodules: recursive`), `git config --global --add safe.directory
  '*'`, cache `build/downloads` (key: hash of `scripts/build-fujinet.sh`
  + `scripts/alpine-env.sh`), then `scripts/build-emulator.sh`,
  `scripts/build-fujinet.sh`, `scripts/mkimage.sh`. Upload
  `build/atari-os-x86-<version>.img.xz` (`xz -T0 -6`) and `SHA256SUMS`
  as the artifact `image`.
- job **host-build**: ubuntu-latest, `apt-get install build-essential
  autoconf automake libdrm-dev libasound2-dev zlib1g-dev`, build the
  emulator against the host the way `build/atari800-host` is built
  (document the exact commands; this is the `-Werror` check that catches
  non-C89 code), and compile `init/init.c` static. No image.
- job **smoke** (needs image): ubuntu-latest, `apt-get install qemu-system-x86
  ovmf`, download the artifact, `unxz`, run `scripts/qemu-test.sh start
  bios` and `start uefi` in turn (the harness must accept the image via
  `QEMU_IMG`), wait up to 180 s for `pidof atari800` over serial (KVM is
  available on GitHub's Linux runners; fall back to TCG if `/dev/kvm` is
  missing and raise the timeout to 600 s), take a screenshot, upload the
  screenshots and `qemu-test` logs as artifacts, fail if the emulator
  never started.

`.github/workflows/release.yml`, on tags `v*`: the same image job, then
`gh release create "$TAG" build/atari-os-x86-*.img.xz build/SHA256SUMS
scripts/flash.sh --generate-notes --title "atari-os-x86 $TAG"` (uses the
default `GITHUB_TOKEN` with `contents: write`). The release body template
(`.github/release-template.md`, inlined by the workflow above the generated
notes) says: what the file is, the three install routes (§5), the minimum
hardware, and the link to KNOWN-ISSUES.md.

Also add `CHANGELOG.md` with an `Unreleased` section listing what exists
today (one line per feature; read README "Status").

## 5. The installer: `scripts/flash.sh`

A single POSIX-shell-compatible **bash** script (bash is required and
checked; macOS ships 3.2, so no bash-4 features) for Linux and macOS.
Shipped as a release asset and in the repo. Windows users are pointed at
balenaEtcher or Rufus (DD mode) in the README; Ventoy users copy the
`.img` (after `unxz`).

Safety is the design, in this order:

1. **Candidates only.** Linux: `lsblk -J -d -o NAME,SIZE,RM,TRAN,MODEL,SERIAL,MOUNTPOINTS,TYPE`
   (fallback without `-J` if the lsblk is old: parse `-P`), keep
   `TYPE=disk` and (`RM=1` or `TRAN` in `usb`,`mmc`); drop any disk that
   has a partition mounted at `/`, `/boot`, `/boot/efi`, `/home`, `/usr`,
   `/var`, or that holds swap, or that is larger than 512 GiB (a USB HDD
   is still allowed below that). macOS: `diskutil list external physical`
   plus `diskutil info -plist` for size/name/removable. Anything not in
   the candidate list cannot be written, period: there is no override
   flag for the device. The only non-disk target allowed is a **regular
   file** when `--to-file PATH` is given (for tests and for Ventoy users
   who want a copy); it must not be a device node.
2. **Show, then ask.** Print a numbered table: device, size, model,
   serial, current mount points. The user must **type the device name**
   (`sdb`, `disk4`), not a number, and then confirm with `yes` after a
   line that repeats size and model. `--device NAME` pre-selects but still
   requires the `yes` unless `--yes` is also given; `--yes` without
   `--device` is an error. `--dry-run` prints the plan and exits 0.
3. **Write.** Unmount every partition of the device (`umount`; macOS
   `diskutil unmountDisk`); use `sudo` only for the unmount, the write and
   the final eject (explain before the first prompt). Input may be `.img`
   or `.img.xz` (stream through `xz -dc`); write with `dd bs=4M
   conv=fsync` (macOS: `bs=4m`, raw device `/dev/rdiskN`), with progress
   (`status=progress` when supported, else a dot every 64 MiB via `pv` if
   present). Refuse to write an image whose size exceeds the device.
4. **Verify.** Read back exactly the image length from the device and
   compare its SHA-256 to the image's (computed while writing via `tee`
   into `sha256sum`/`shasum -a 256`, or in a second pass of the input).
   Mismatch = non-zero exit and a red message. Then `sync`, eject (`eject`
   / `diskutil eject`) and print the next steps (boot menu key, F8 = System
   menu, Ventoy note).

Also: `--help`; `set -euo pipefail`; every external tool checked up front
(`xz`, `lsblk`/`diskutil`, `dd`, `sha256sum`/`shasum`); clear error when
run on an unsupported OS; no `rm -rf`, no `mkfs`, no partition editing.
Tests: `tests/flash-test.sh` runs the script with `--to-file` on a
scratch file for `.img` and `.img.xz` inputs and checks the verify step
(corrupt the file between write and verify in a test hook env var
`FLASH_TEST_CORRUPT=1` → must fail). Shellcheck-clean (`shellcheck` is in
Alpine: run it through `alpine-env.sh run`).

README gets an **Install** section (first thing after the intro): download
the latest release, three routes (Etcher/Rufus on Windows; `flash.sh` on
Linux/macOS with a one-line example; Ventoy), how to boot from USB, and
that the stick stays writable (your library lives on it). Note the 1 GiB
image size and that the rest of the stick is unused for now (a later spec
grows the partition on first boot).

## 6. Not in this spec

- Growing the FAT partition to the stick size on first boot.
- Signing the image. A LICENSE file (Carlos picks the licence; atari800 is
  GPL-2.0-or-later, so our code can't be more restrictive than compatible).
- Pushing the repo to GitHub and creating the first tag: Carlos does this;
  the workflows must be correct on the first run, so the agent validates
  them with `actionlint` if available (`apk add actionlint` in the Alpine
  env or the Go binary) and by reading the actions' documented inputs.
