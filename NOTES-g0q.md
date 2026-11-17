# ios-sweet -> Galaxy S22+ (g0q) port notes

Base project: https://github.com/dipa-studio/ios-sweet (Redmi Note 10 Pro, SM7150).
Guide: https://hackiosx.dev/porting

## Phone (read over adb, 2026-11-17)

- Model SM-S906E, device g0q, board "taro", region INS (CSC OXM)
- Running build S906EXXSCFYI2, Android 15, security patch 2025-09-01, kernel 5.10.226
- SoC SM8450 "waipio" (soc_id 457), revision 2.2; board revision 13
- Bootloader unlocked: ro.boot.flash.locked=0, verifiedbootstate=orange
- Bootloader picked dtb_idx=0, dtbo_idx=4
- ~7.4 GB RAM visible to Android
- Not rooted: /sys/firmware/fdt, /proc/device-tree/* and /proc/cmdline are unreadable from adb shell

## Firmware on disk (NOT the phone's exact build)

- `firmware/SM-S906E_..._fac.zip` = S906EXXUCGYJ1 (Oct 2025, Android 16, bootloader rev C).
  Samsung's server sent this instead of the requested SCFYI2.
- Used ONLY to read the hardware description. Never flash the BL from it.
- `firmware/ap/`: boot, vendor_boot, dtbo, recovery, vbmeta, vm-bootsys (decompressed)
- boot.img and vendor_boot.img are header v4 (page 4096). vendor_boot name SRPUI14B012
- `firmware/dtb/dtb00.dts` = base tree "Waipio v2", msm-id <0x1C9 0x20000>
- `firmware/dtbo/dtbo04.dts` = overlay "Samsung G0Q PROJECT (board-id,12)"

## Hardware facts (from the device trees)

| Block | S22+ (SM8450) | Redmi sweet (SM7150), for comparison |
| --- | --- | --- |
| GICv3 dist / redist | 0x17100000 / 0x17180000 (+0x200000), ITS 0x17140000 | 0x17a00000 / 0x17a60000 |
| Arch timer | PPIs 13, 14, 11 (virt), 12; 19.2 MHz. Virtual timer INTID 27 | virt INTID 19 |
| PSCI | psci-1.0, smc | smc |
| CPUs | 8, MPIDR Aff1 0x000..0x700 (4x A510, 3x A710, 1x X2) | Aff1 scheme, same |
| UFS host | 0x1D84000 (+0x3000), SPI 265 | 0x01d84000 (same address) |
| Memory node | reg = 0 in firmware, filled in by the bootloader at boot | |
| Reserved memory | all below 0xEA000000; hyp_region (Gunyah) at 0x80000000 | |
| Splash buffer | cont_splash_region 0xB8000000, size 0x2B00000 | 0x9c000000 |
| Panel | Samsung S6E3FAB AMB667XU01, 1080x2340, **DSI command mode** (needs a display refresh kick) | video mode |
| Touch | STM FTS over SPI ("stm,stm_ts_spi") | Goodix GT9896 over I2C |
| Keys | power = PON KEY_POWER (0x74), vol-down = PON RESIN (0x72), vol-up = gpio-keys (0x73) | |
| SPMI | PMIC arbiter, two buses (0xC42D000, 0xC432000), newer arbiter than sweet | |

## Port impact

- Address-only changes: GIC, timer INTID, memory range, splash address, UFS (same address).
- New work: Samsung boot path (no fastboot; Odin/Heimdall flash to BOOT, header v4),
  command-mode display refresh, STM FTS SPI touch driver, newer SPMI arbiter for keys,
  USB networking addresses, Gunyah hypervisor considerations.
- Memory: the real DDR layout must come from the live FDT (the shim reads it at boot) or root.

## Open decisions

- Flasher: Odin (Windows) vs Heimdall built on this Mac.
- Restore path: no exact SCFYI2 boot.img backup exists (no root). Fallback = full stock flash of CGYJ1.
- Kernel build: full Xcode on this Mac vs GitHub Actions macOS runner.

## Boot log

### Boot 1 — 2026-11-17, milestone 1 (commit 305c717), WORKED
Flashed `shim-boot-g0q.img` (sha256 af256fc6…0fc8) to BOOT with Heimdall. Knox tripped (user accepted).
Stock bootloader accepted the header v4 image with no vbmeta change. Screen photo:
- EL1, boot CPU MIDR 0x410FD462 (Cortex-A510), MPIDR 0x81000000 (MT=1, core id in Aff1)
- ID_AA64MMFR0: TGran16 = 1 (16K pages OK), PARange = 2 (40-bit)
- ID_AA64ISAR1: APA = 5 (PAuth present), API = 0
- Live DTB at 0xB6934000 (x0), magic OK; CNTFRQ 19.2 MHz
- MDP hw 0x80010000 (SDE 8.1); CTL0 INTF_ACTIVE 0x2 (INTF_1/DSI0)
- Pipe VIG0 src size 540x2340, SRC0_ADDR 0xB8000000, stride 4320, format 0x000237FF (32bpp)
  -> full 1080-wide buffer; the right half is fetched by a second rect/pipe
- INTF_1 tear check on; autorefresh 0x80000001 works: heartbeat + TE counter advance on screen
- PSCI 1.1, SMCCC 1.1

### Boot 2 — 2026-11-17, milestone 2 (commit 259eec2), CRASHED
Blue panic, stage "gic vtimer": SYNC, ESR 0x02000000 (EC 0 = unknown/undefined),
ELR 0xA81222E4. Shim image was loaded at 0xA8120000 (X0 = "VTIMER" string at +0x46A7).
Offset 0x22E4 = `msr CNTP_CTL_EL0` in irq_window cleanup.
**Gunyah traps EL1 physical-timer access on SM8450.** Use the virtual timer only
(CNTV_*, CNTVCT). XNU patches must be checked for any CNTP/CNTPCT use before the first kernel boot.
The panic screen cleared the memory/SMP results, and the panic reset loops back into the shim.
Fix in a41a8c9: no CNTP access on g0q, panic prints below the results and halts.

### Boot 3 — 2026-11-17, milestone 2 fixed (commit a41a8c9), WORKED except CPU1 GICR probe
Live FDT /memory (bootloader-filled), MiB: 0x80000000+1696, 0xF1C00000+228, 0x800000000+939,
0x83B100000+30, **0x840000000+5120 (5 GiB contiguous, candidate XNU RAM)**. Total 8013 MiB.
SMP: PSCI CPU_ON works for all 7 secondaries. MPIDR Aff1 = 1..7.
CPU0-3 Cortex-A510 (0x410FD462), CPU4-6 Cortex-A710 (0x412FD470), CPU7 Cortex-X2 (0x412FD480).
GIC: GICD_CTLR 0xD0 -> 0xD3; GICR frame for CPU0 at 0x17180000, waker 0.
VTIMER (INTID 27): 30 IRQs / 300 ms (exact). SGI1: 10/10. ICC_CTLR 0x88400.
CPU1 crashed in its GICR probe: ESR 0x96000010 (sync external abort), FAR 0x171A0008.
Cause: **redistributor stride is 0x40000 (GICv4 VLPI frames)**, DT redistributor-stride = 0x40000.
XNU patch 0004 hard-codes GICR_PE_SIZE 0x20000: must become 0x40000 for g0q.

### Kernel port checklist (from boots 1-3)
- Virtual timer only (Gunyah traps CNTP_* at EL1); vtimer INTID 27
- GICD 0x17100000, GICR 0x17180000, stride 0x40000
- RAM for XNU: 0x840000000 + 5 GiB (check reserved list from milestone 3)
- 16K granule OK, PA 40-bit; boot CPU A510; 8 cores via PSCI with MPIDR Aff1
- Framebuffer: bootloader buffer at 0xB8000000, stride 4320, 1080x2340, needs INTF_1 autorefresh

### Boot 4 — 2026-11-17, milestone 3 (commit cadfc64), WORKED; UFS commands refused
Reserved memory above 4 GiB (live FDT, 48 static regions total): sec_debug_region x2,
google_debug_kinfo, sec_qcom_rdx_bootdev (173 MiB), hdm_region — all within
0x800000000-0x80B700000 (bank 3). **Bank 0x840000000 + 5 GiB has no reservations.**
CPU1 GICR found=1 with stride 0x40000 (fix confirmed).
UFS host at 0x1D84000: UFSHCI 3.0, CAP 0x1587031F (32 slots), HCE=1, HCS=0x10F
(device present, lists ready, UPMCRS=1), IS=0x400 (link startup done), DBR=0.
Bootloader's UTRL at 0xFF0B3000; ours taken over (0xA8086000).
TUR x3, READ CAPACITY, READ(10) on LUN 0: rc=4, OCS=0, SCSI status 2 (CHECK CONDITION).
-> host, link and DMA through the SMMU work; the device refuses. Need sense data (milestone 4).

### Boot 5 — 2026-11-17, milestone 4 (commit cfbd7f5), WORKED: UFS solved
Before wake: REPORT LUNS -> ILLEGAL REQUEST (5/00/00); INQUIRY and TUR on LUN0-7 -> NOT READY (2/00/00).
**The S22+ bootloader leaves the UFS device in a low-power mode.**
START STOP UNIT (POWER CONDITION 1 = Active) to the UFS Device W-LUN (UPIU LUN 0xD0): rc 0.
After wake, 4 KiB blocks:
- LUN0 lastlba 0x01DA03FF -> 118.5 GiB (128 GB model), main LUN (userdata here)
- LUN1 0x7FF (8 MiB), LUN2 0x7FF (8 MiB), LUN3 0x3FF (4 MiB), LUN4 0x27FFF (640 MiB), LUN5 0xFFF (16 MiB)
- LUN6/7: ILLEGAL REQUEST 5/25/00 (logical unit not supported)
**XNU UFS driver (patches 0028/0030/0069) must send this SSU before any I/O on g0q.**
Bootloader UTRL moves between boots (0xFF0B3000, 0xFF0B4000); the shim loads at ~0xA80xxxxx-0xA81xxxxx.

### Boot 6 — 2026-11-17, milestone 5 (commit dd9f226), WORKED: GPT on LUN 0
SSU Active rc 0, LUN0 TUR OK (reproducible). MBR 0xAA55 type 0xEE, GPT "EFI PART", 44 entries at LBA 2.
Partitions >= 64 MiB (start LBA in 4 KiB blocks):
dsp 0xA1F0 64M; apnhlos 0xF1F0 150M; modem 0x1CB00 164M; boot 0x26C00 96M; recovery 0x2CC00 100M;
vendor_boot 0x33000 96M; vm-bootsys 0x39000 335M; super 0x4DF00 11500M; prism 0x31CB00 1000M;
cache 0x35C700 800M (ext4); core_nhlos_a 0x396600 64M;
**userdata 0x39A800 (3778560) 0x1A05B MiB = 104 GiB**, fs unreadable (Android metadata encryption).
-> iOS root goes at the start of userdata (LUN 0, LBA 3778560), as on sweet.

## Hardware survey complete (boots 1-6). Kernel port values for g0q:
| | g0q |
| --- | --- |
| Entry | EL1 under Gunyah, x0 = live FDT; image loaded ~0xA81xxxxx |
| RAM for XNU | 0x840000000 + 5 GiB (no reservations) |
| GIC | GICD 0x17100000, GICR 0x17180000 stride 0x40000 |
| Timer | virtual only, PPI 11 = INTID 27, 19.2 MHz; CNTP_* traps |
| CPUs | 8 via PSCI 1.1 CPU_ON, MPIDR Aff1 0..7; A510 x4, A710 x3, X2 |
| Display | buffer 0xB8000000, 1080x2340, stride 4320, 32bpp; INTF_1 (0xAE36000) autorefresh |
| UFS | 0x1D84000, link up; START STOP UNIT Active to W-LUN 0xD0 first; root on LUN 0 LBA 3778560 |

## Kernel build: blocker found (2026-11-17)
ios-sweet's CI builds xnu-8796 (macOS 13.5) on GitHub macos-14 runners with Xcode 15.x on purpose:
Xcode 16+ `iig` emits IORPC.kernelContent, which xnu-8796 lacks. This Mac has Xcode 27.2.
blacktop/darwin-xnu-build @663906528 also needs the Kernel Debug Kit 13.5 (22G74) installed in
/Library/Developer/KDKs (Apple Developer download with an Apple ID, admin install), plus jq, gum,
xcodes, cmake, ninja, ipsw. Options: GitHub Actions on a fork (proven), or Xcode 15.4 + KDK 13.5 locally
(Apple ID downloads; Xcode 15 may not run on macOS 27).
GitHub Actions (2026-11-17): macos-14 label rejected ("not supported"), macos-15 jobs never acquired,
macos-26 image has only Xcode 26.0-26.6. **No Xcode 15 anywhere** -> xnu-8796 must be made to build
with a current Xcode (fix iig/IORPC.kernelContent). Building locally on this Mac (Xcode 27.2).
Fork: github.com/deepakbeebom/ios-sweet (Actions enabled; probe branch deleted).
KDK 13.5 (22G74) needed: Apple Developer download, unpack to ~/ios-16 and pass KDKROOT.

## Kernel builds locally (2026-11-17)
Baseline SWEET kernel built on this Mac with Xcode 27.2: dxb/fakeroot/System/Library/Kernels/
kernel.development.sweet (16.7 MB, sha256 090c16e1…f71e). Needed: patch 0082 (IORPC.kernelContent
for current iig), KDK 13.5 unpacked at ~/ios-16/kdk, stand-ins in ~/ios-16/bin, build.sh tweaks.
Reproduce: KDKROOT=~/ios-16/kdk/KDK_13.5_22G74.kdk TOOLS_BIN=~/ios-16/bin ios-sweet/tools/build-xnu-mac.sh ~/ios-16/xnu-work
Incremental rebuild after the first run takes a few minutes.

## g0q kernel changes needed (found in the patched tree)
- 0004: SWEET_VTIMER_INTID 19 -> 27; GICR_PE_SIZE 0x20000 -> 0x40000
- 0010/0012: early framebuffer 0x9c000000 -> 0xb8000000, 1080x2400 -> 1080x2340 (+ INTF_1 autorefresh)
- 0046/0061: panel 1080x2400 -> 1080x2340; iPhone X 1125x2436 window offsets
- osfmk/arm64/machine_routines.c _enable_virtual_timer(): skip the CNTP_CTL_EL0 write (traps under Gunyah)
- user timebase: check whether EL0/EL1 CNTPCT_EL0 reads trap before advertising it
- UFS: START STOP UNIT (Active) to W-LUN 0xD0 before I/O; root at LUN 0 LBA 3778560
- mkadt.py: RAM 0x840000000 + 5 GiB, GIC/timer/CPU (8 cores, Aff1), framebuffer
- new drivers: STM FTS touch over SPI, keys over the SM8450 SPMI arbiter (PMK8350 PON, gpio-keys)

### Milestone 6 prepared (commit on g0q branch, 2026-11-17)
Kernel: patch 0083 applied, kernel.development.sweet sha256 794db635…2d20 (16.7 MB).
ADT: mkadt.py --g0q. Payload cmdline (read-only, 1 CPU):
  debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=userdata
  wdt=-1 wdt_test=no_user_checkin sweetkeys=0 sweettouch=0 sweetusb=0
Boot image shim/out-g0q/shim-boot-G0Q_M6.img sha256 bf57ed9c…0f43 (16.9 MB).
No iOS root yet: expected to stop at the root mount (userdata is Android, encrypted).
