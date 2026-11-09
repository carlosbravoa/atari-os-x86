# Spec: Wi-Fi firmware for every free driver, and the 32-bit UEFI loader

Status: Carlos, 2026-11-09: "That is the plan for the night, unattended."
Design: Claude (Fable); implementer: an Opus agent; verification: Claude in
QEMU (no Wi-Fi hardware there: the files and the boot are what can be
checked), Carlos's community on real laptops. Rules as in
`docs/specs/menus.md` §0 and `docs/specs/library.md` §3/§7. Nothing is
committed by the agent.

## Why

The modloop has the kernel driver for every laptop Wi-Fi family but
firmware only for Intel (`FIRMWARE_PKGS` in `scripts/mkimage.sh`). On an
Atheros ath10k, Realtek, Broadcom or MediaTek laptop the driver loads,
finds no firmware and the machine has no Wi-Fi, silently. Package sizes
(Alpine 3.24, download size, which is close to the squashfs size):

| Package | MB | Hardware |
|---|---|---|
| linux-firmware-ath10k | 9.2 | QCA9377, QCA6174, QCA988x: 2015-2020 HP, Lenovo, Dell |
| linux-firmware-ath9k_htc | 0.1 | USB Atheros sticks |
| linux-firmware-rtlwifi | 0.6 | RTL8723BE/8188EE/8192EE/8821AE: 2013-2018 budget laptops |
| linux-firmware-rtw88 | 0.4 | RTL8821CE/8822BE/8822CE: 2018-2021 |
| linux-firmware-rtw89 | 5.2 | RTL8852AE/BE: 2021+ |
| linux-firmware-brcm | 8.2 | BCM4356/43602/4350, MacBooks, older HP/Dell (brcmfmac, brcmsmac) |
| linux-firmware-mediatek | 29.6 | MT7921/MT7922 2021+ AMD laptops (mt76), plus a lot that is not a laptop |
| linux-firmware-rtl_nic | 0.1 | Realtek Ethernet (r8169 wants it on some chips) |

Not added, on purpose: ath11k/ath12k (Wi-Fi 6/7, 40 MB, not "old
laptops"), Bluetooth firmware (no Bluetooth stack), cirrus (2022+
amplifiers), Qualcomm/Marvell/Mellanox.

## A. Firmware

1. `FIRMWARE_PKGS` gains `linux-firmware-ath10k linux-firmware-ath9k_htc
   linux-firmware-rtlwifi linux-firmware-rtw88 linux-firmware-rtw89
   linux-firmware-brcm linux-firmware-mediatek linux-firmware-rtl_nic`.
   `scripts/alpine-env.sh bootstrap` installs them in the build
   environment if that is where the stage gets them (read how the
   current firmware packages reach `$STAGE/lib/firmware`; follow the same
   path).
2. Prune in the firmware step of `mkimage.sh` (next to the nvidia GSP and
   iwlwifi pruning), each with a one-line comment:
   - `mediatek/`: keep only the mt76 Wi-Fi files: `WIFI_RAM_CODE_MT79*`,
     `WIFI_MT79*`, `mt7961*`, `mt7925*` (if present), `mt7663*`, `mt7615*`,
     `mt7650*`, `mt7610*`, `mt76x0*`, `mt76x2*`, `mt7668*`, `mt7601*`; drop
     `BT_RAM_CODE_*` (Bluetooth), `mt7622*`, `mt7915*`, `mt7981*`,
     `mt7986*`, `mt7988*`, `mt8*`, `sof*`, and anything else. Target under
     10 MB; report the exact size.
   - `brcm/`: drop the SDIO variants for boards and phones
     (`brcmfmac*-sdio.*` and their `.txt`/`.clm_blob`), keep PCIe
     (`brcmfmac*-pcie.*`) and USB (`brcmfmac*.bin` without a bus suffix
     is USB for 43236/43242/43569: keep), plus `bcm43xx-0.fw`,
     `bcm43xx_hdr-0.fw`, `brcmfmac43602-pcie.*`, `brcmfmac4350*`,
     `brcmfmac4356*`, `brcmfmac4358*`, `brcmfmac4366*`, `brcmfmac4371*`
     and the Apple `brcmfmac4364*`/`brcmfmac4377*`/`brcmfmac4378*`
     PCIe sets (MacBooks are the main Broadcom laptop population). Report
     the size.
   - `ath10k/`: drop `WCN3990` (phones). Keep the rest.
   - Remove the Intel QuickAssist files that `linux-firmware-intel` drops
     at the top of `/lib/firmware` (`qat_*.bin*`): datacenter only.
3. The modules were already in the modloop: confirm `ath10k_pci`,
   `rtl8723be`, `rtw88_8821ce`, `rtw89_8852ae`, `brcmfmac`, `brcmsmac`,
   `mt7921e` all still are (the prune list must not have lost them) and
   that `modinfo -F firmware` of each names files that now exist in the
   modloop (do this against `build/modloop/`, a loop over the module's
   firmware list; QEMU has no such hardware).
4. Size: report the modloop before and after (`ls -l
   build/stick/atari-os/modloop.sqfs`); expected about +25 MB. The safety
   check against `docs/specs/zenbook-modules.txt` must still pass.

## B. 32-bit UEFI loader (BOOTIA32.EFI)

Cheap Atom laptops and tablets from 2013-2016 (Bay Trail, Cherry Trail)
have 32-bit UEFI firmware on a 64-bit CPU and cannot see a stick that only
has `BOOTX64.EFI`. Alpine's `limine-efi-x86` package has
`/usr/share/limine/BOOTIA32.EFI`; the Alpine `linux-lts` x86_64 kernel is
built with `CONFIG_EFI_MIXED`.

1. `scripts/alpine-env.sh`: add `limine-efi-x86` to the packages.
2. `scripts/mkimage.sh`: copy `BOOTIA32.EFI` next to `BOOTX64.EFI` into
   `EFI/BOOT/`. Wherever the x86_64 build copies or lists `BOOTX64.EFI`
   for the installers (the files zip, `scripts/install-linux.sh`,
   `scripts/install-windows.ps1`, the F8 Install in `init/init.c`), do the
   same for `BOOTIA32.EFI` (grep for BOOTX64).
3. Test. Ubuntu has no 32-bit OVMF package. Get one without trusting it
   more than needed: the Alpine `ovmf` package (check with `apk fetch` in
   the build environment whether it has an IA32 build; it likely has only
   x64), else download Arch Linux's `edk2-ovmf` package
   (`https://archlinux.org/packages/extra/any/edk2-ovmf/download/`, a
   `.pkg.tar.zst`) into a fresh directory under the scratchpad
   (`/tmp/claude-1000/-home-carlos-devel-atari-os-x86/f4379cf0-7af1-496e-9383-c82069571e87/scratchpad/ovmf32/`),
   extract only `usr/share/edk2/ia32/OVMF_CODE.4m.fd` and
   `OVMF_VARS.4m.fd`, and keep them in `build/ovmf32/` (small, two files).
   Add a `uefi32` firmware choice to `scripts/qemu-test.sh start` that uses
   them (`qemu-system-x86_64` still: the CPU is 64-bit, the firmware
   32-bit). Boot the image with it. Report whether Limine's IA32 loader
   boots our 64-bit kernel to the Atari (screenshot), or where it stops
   (Limine screen, kernel panic, blank). Either result is useful; if it
   fails, keep the loader anyway only if Limine shows its own error (so a
   user sees something), otherwise leave the file out and say so.

## Docs

- README "Hardware": a Wi-Fi line listing the supported families
  (Intel, Atheros, Realtek, Broadcom with a free driver, MediaTek mt76)
  and that BCM43142/BCM4352 have no free driver (use a USB adapter); the
  32-bit UEFI line if B works.
- CHANGELOG under Unreleased; KNOWN-ISSUES: update the modloop size line
  and the "drivers left out" list (Wi-Fi 6 Atheros firmware not included;
  Bluetooth absent).

## Verification

QEMU BIOS and UEFI boot as before (`scripts/qemu-test.sh`, debug copy of
the image, never `debug-image build/atari-os.img`): the Atari comes up,
`ls /lib/firmware/ath10k/QCA9377 /lib/firmware/rtlwifi /lib/firmware/rtw88
/lib/firmware/brcm /lib/firmware/mediatek | head -40` over `serial` shows
the files, `/lib/firmware/qat_*` is gone, `ls /lib/firmware/mediatek | wc -l`
is small. `uefi32` result per B.3. Delete the debug copies and anything
large you created in `build/` (the user watches its size).
