# Spec: module archive stage 2, drop drivers old laptops never load

Status: Carlos's session 2026-11-08 ("Let's continue with your backlog").
Stage 1 (firmware) took the modloop from 414 to 242 MB; the 131 MB of
kernel modules (.ko.gz) are next. Design: Claude (Fable); implementer: an
Opus agent; verification: Claude (QEMU) and the ZenBook. Rules as in
`docs/specs/menus.md`. Nothing in `scripts/flash.sh`; `flash-it.sh`
untouched. Only `scripts/mkimage.sh` (modloop step 3) and docs change.

## A. What goes

Whole directories under `modules/<kver>/kernel/`, removed after the copy
into `$BUILD/modloop/modules` and before mksquashfs:

- `drivers/media` (TV tuners, webcams, DVB; nothing here uses a camera)
- `drivers/infiniband`, `drivers/md` (RAID/LVM), `drivers/mtd`,
  `drivers/iio`, `drivers/xen`, `drivers/vhost`, `drivers/vfio`,
  `drivers/vdpa`, `drivers/ntb`, `drivers/rapidio`, `drivers/fpga`,
  `drivers/greybus`, `drivers/staging`, `drivers/isdn`, `drivers/atm`,
  `drivers/uio`, `drivers/accel`, `drivers/crypto` (hardware crypto
  accelerators), `drivers/dca`, `drivers/mailbox`, `drivers/mcb`,
  `drivers/memstick`? no: **keep memstick** (Sony card readers on old
  Vaios), `drivers/siox`, `drivers/slimbus`, `drivers/soundwire`? no:
  **keep soundwire** (SOF audio on recent Intel laptops may use it),
  `drivers/target`, `drivers/firewire`? **keep** (old laptops have it, a
  FireWire SIO2PC may appear), `drivers/block` only `drbd`, `nbd`, `rbd`,
  `zram`? keep zram; drop `drbd nbd rbd pktcdvd`.
- `drivers/scsi`: the SAN/RAID HBAs: `qla2xxx lpfc mpt3sas mpi3mr megaraid
  aacraid bfa csiostor be2iscsi fnic snic qedf qedi isci pm8001 mvsas
  arcmsr aic7xxx aic94xx smartpqi hpsa cxgbi elx libfc fcoe libsas
  hisi_sas esas2r dpt_i2o sym53c8xx_2 3w-9xxx 3w-sas 3w-xxxx advansys
  hptiop initio ipr ips mpt3sas myrb myrs stex wd719x fdomain qlogicfas408
  qla1280 dc395x am53c974 BusLogic a100u2w atp870u`, plus
  `scsi_transport_fc scsi_transport_sas scsi_transport_iscsi iscsi_tcp
  libiscsi libiscsi_tcp`. Keep `sd_mod sr_mod sg st ch scsi_mod
  scsi_common sd_mod ses virtio_scsi vmw_pvscsi?` (drop vmw_pvscsi) and
  everything not named.
- `drivers/net/ethernet`: the datacenter vendors `mellanox intel/ice
  intel/i40e intel/iavf intel/fm10k intel/ixgbe intel/ixgbevf intel/idpf
  broadcom/bnxt broadcom/bnx2x chelsio qlogic cavium netronome emulex
  solarflare huawei pensando fungible microsoft wangxun amd/xgbe
  amd/pds_core sfc brocade cisco myricom neterion tehuti`, and
  `drivers/net/{fddi hippi wan hamradio ieee802154 can arcnet plip team
  bonding ipvlan macvlan? (keep macvlan: no) ppp slip}`; keep `wireless`,
  `usb`, the consumer ethernet (intel/e1000 e1000e igb igc, realtek,
  atheros, broadcom/tg3 b44, marvell, nvidia, via, jme, sis, 3com, dlink,
  dec, 8390, smsc, renesas? drop renesas, aquantia keep), `phy`, `mdio`,
  `tun`, `veth`, `dummy`, `bridge`.
- `fs`: `ocfs2 gfs2 dlm ceph nfs nfsd nfs_common lockd afs orangefs 9p coda
  ubifs jffs2 nilfs2 reiserfs ufs minix bfs qnx4 qnx6 omfs befs adfs affs
  efs freevxfs sysv hpfs hfs hfsplus jfs cramfs romfs zonefs bcachefs
  erofs f2fs`. Keep `ext4 jbd2 mbcache btrfs xfs fat vfat exfat ntfs3
  isofs udf squashfs fuse overlayfs autofs4 smb nls binfmt_misc
  configfs efivarfs pstore` and the rest.
- `net`: `netfilter ipv4/netfilter ipv6/netfilter bridge/netfilter sched
  dccp sctp rds tipc batman-adv openvswitch mpls nsh l2tp caif ceph
  can ieee802154 6lowpan mac802154 nfc rxrpc smc xfrm? keep xfrm`. Keep
  `wireless mac80211 bluetooth rfkill 802 ipv4 ipv6 packet unix key
  8021q llc sunrpc? drop sunrpc with nfs` and the rest.

Everything else stays. When in doubt, keep.

## B. Consistency

After deleting, regenerate `modules.dep`, `modules.alias`,
`modules.symbols`, `modules.builtin.*`, `modules.order` for the pruned
tree: `depmod -b <tmp> <kver>` over a temp tree where the pruned modules
sit at `<tmp>/lib/modules/<kver>` (the Alpine env has `kmod`; `apk add
kmod` to BUILD_PKGS in `scripts/alpine-env.sh` if missing), then copy the
`modules.*` files back. Both `.ko.gz` and the initramfs's `.ko.xz` must
still resolve (the initramfs has its own `modules.dep`, untouched).

## C. Safety check (must pass in mkimage, fail the build otherwise)

1. Every module named in `docs/specs/zenbook-modules.txt` (182 modules the
   ZenBook loads) still exists in the pruned tree.
2. Every module of `base_modules[]` in `init/init.c` exists.
3. This keep-list exists (old laptops in the wild): `ath9k ath10k_pci ath11k_pci
   ath5k rtw88_pci rtw88_8822be rtw88_8822ce rtw89_pci rtl8xxxu rtl8192ce
   rtl8192cu rtl8188ee rtl8723be rtl8821ae rt2800pci rt2800usb brcmfmac
   brcmsmac b43 b43legacy mt76x2e mt7921e mt7925e iwlwifi iwlmvm iwldvm
   iwl3945 iwl4965 ipw2200 r8169 e1000e e1000 igb igc tg3 b44 sky2 skge
   atl1c atl1e atl1 alx forcedeth via_rhine via_velocity jme r8152 asix
   ax88179_178a cdc_ether rndis_host radeon amdgpu nouveau i915 xe
   snd_hda_intel snd_hda_codec_realtek snd_hda_codec_conexant
   snd_hda_codec_hdmi snd_hda_codec_idt snd_hda_codec_sigmatel
   snd_hda_codec_via snd_hda_codec_cirrus snd_sof_pci_intel_tgl
   snd_sof_pci_intel_cnl snd_sof_pci_intel_apl snd_sof_pci_intel_icl
   snd_sof_pci_intel_mtl snd_sof_pci_intel_lnl snd_sof_amd_renoir
   snd_sof_amd_rembrandt snd_acp3x_pdm_dma snd_intel8x0 snd_ens1371
   snd_usb_audio usb_storage uas sdhci sdhci_pci rtsx_pci rtsx_pci_sdmmc
   rtsx_usb rtsx_usb_sdmmc mmc_block memstick thinkpad_acpi asus_wmi
   asus_nb_wmi dell_laptop dell_wmi hp_wmi ideapad_laptop acer_wmi
   samsung_laptop toshiba_acpi sony_laptop fujitsu_laptop msi_wmi
   lg_laptop panasonic_laptop xpad joydev hid_sony hid_microsoft hid_logitech
   hid_logitech_dj hid_nintendo hid_playstation hid_steam hid_multitouch
   i2c_hid i2c_hid_acpi psmouse synaptics_usb elan_i2c atkbd coretemp
   k10temp k8temp fam15h_power acpi_cpufreq intel_pstate? (built in; skip
   if missing) processor_thermal_device pcc_cpufreq btusb btintel btrtl
   btbcm ath3k usbhid hid_generic evdev uinput? (keep) loop squashfs vfat
   exfat ntfs3 ext4 btrfs xfs nvme ahci ata_piix pata_acpi nls_cp437
   nls_iso8859_1 nls_utf8 nls_cp850 nls_cp1252 efivarfs efi_pstore
   firewire_ohci firewire_sbp2 ohci_pci ehci_pci xhci_pci uhci_hcd
   pcspkr? (keep) cdc_acm ftdi_sio pl2303 cp210x ch341 usbserial`
   (serial adapters for a real SIO2PC later). A name that does not exist
   in the unpruned tree either (kernel config) is reported but does not
   fail the build; a name removed by the prune fails it.
4. `modprobe -n -d <tree>` resolution is not available for the modloop
   layout; instead check `modules.dep` has an entry for each name and that
   every dependency it lists exists.

## D. Numbers and docs

Log `modloop modules: <before> -> <after> MB` and the final squashfs size.
README "Hardware" or "Building" paragraph: what classes of drivers are
left out and how to get them back (comment out the prune list in
mkimage.sh). CHANGELOG. KNOWN-ISSUES: no TV tuners, no FC/SAS HBAs, no
cluster/network filesystems, no datacenter NICs.

## E. Verification (Claude)

QEMU boots the debug image (virtio, e1000e, bochs, xhci, usb-storage all
fine), Library sees the USB test drive, FujiNet comes up; the ZenBook
boots with every one of its 182 modules loaded (lsmod count equal) and
Wi-Fi, sound and the touchpad working.
