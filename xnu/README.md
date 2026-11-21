# XNU patch series

`xnu/patches/` holds the **complete** series the lavender kernel is built from. CI hands it to
[`blacktop/darwin-xnu-build`](https://github.com/blacktop/darwin-xnu-build) on top of Apple's open-source `xnu-8796` (see `docs/BUILD.md`).

- `0001`–`0192` come from [`aLpHa-Git-69/ios-lavender`](https://github.com/aLpHa-Git-69/ios-lavender), commit `bc606b7`, which is the pinned base `83356567…` plus the 30 s `0192`. That is the series that built the lock-screen kernel. `ios-lavender` is shared with other devices, so it is not modified; this repository is now where lavender changes are made.
- `0019-stage-libpthread.sh`, `0020-stage-bearssl.sh` and `0026-stage-hfs.sh` stage sources the build needs.
- The series also carries the sweet/topaz/santoni/whyred history (`0003`–`0141`, `0148`, `0176`–`0188`). Lavender builds on those changes, so they stay in place even where the names say another board.

## Lavender patches

| Patch | Purpose | Status |
| --- | --- | --- |
| `0145`–`0147` | `LAVENDER` machine config, the framebuffer and the SDCC v5 root | On device |
| `0189`, `0190` | SDHCI exact PIO + cache state; GIC RWP wait and reset cleanup | On device |
| `0191-lavender-touch-stub-common-headers.patch` | Shared `santoni_touch_probe()` entry point built as an unsupported-board stub | On device (stub) |
| `0192-sdhci-wait-card-ready-after-write.patch` | Wait for DAT0 + `TRAN`/`READY_FOR_DATA` (CMD13) after every PIO write; the eMMC write fix | On device: reached the lock screen |
| `0200-lavender-usb-gadget.patch` | SDM660 USB device (DWC3) with the CDC-ECM network and live log; starts the input and USB drivers on lavender | On device (`sweetusb=1`): controller powered and clocked, in device mode; PHY powered down, so it needs `0210` |
| `0201-lavender-pmic-keys.patch` | Power / Volume Up / Volume Down through the SDM660 SPMI arbiter v3 | **On device**: all three keys reach `SWEET HID` (log `sweetlog-usbkeys-0202`) |
| `0210-lavender-usb-phy-power-and-hold.patch` | Power up the QUSB2 v1 PHY that ABL leaves powered down (`sweetusb>=2`); `sweetusbhold=1` USB test boot that stops before the root mount | On device: PHY PLL locks in 10 us, hold mode works; ep0 `SETEPCONFIG` still timed out |
| `0211-lavender-dwc3-core-bringup.patch` | Linux/bergamota DWC3 bring-up: UTMI clock as PIPE clock, core+PHY soft reset, HS quirks (`sweetusbfix` mask, default 7) | On device: no more endpoint timeout, core runs, one bus-reset event; link stays suspended |
| `0212-lavender-qusb2-utmi-mux.patch` | Release the QUSB2 UTMI test mux (`UTMI_CTRL2 0x40`) ABL leaves set, so the controller drives the D+ pull-up (`sweetusbutmi=0` keeps it) | **On device: enumerates on Windows at high speed** (`VID_1209&PID_0001`, CDC-ECM, no Windows driver) |
| `0213-lavender-usb-ncm.patch` | CDC-NCM function (default on lavender, `sweetusbncm=0` = ECM) so Windows binds its in-box UsbNcm driver; NTB16 framing; SPEED_CHANGE; bus reset handling | **On device**: Windows 11 UsbNcm adapter up, ping 70/70, live log |
| `0215-lavender-fb-swap-telemetry.patch` | Log the first `sweetfblog` commits and every change of the enabled layer set; the old `SWEET FB: swap` detail stopped at 16 presents and the counter only every 1000 commits, which made an idle lock screen look frozen | Kernel only; the lock-screen boot with this series is the test |
| `0216-lavender-rtc-calendar-base.patch` | Calendar base from `sweetrtc=<unix seconds>` when the platform clock answers 0 (the PM660 RTC is not read yet); the shim passes its build time | Kernel only; look for `SWEET RTC: calendar base` and a 2026 date on the lock screen |
| `0217-lavender-sweetnopb.patch` | `sweetnopb=1` makes the exec of `PosterBoard` fail with EPERM, to isolate the poster crash from the rest of the lock screen | Diagnostic, off by default |

| `0220-lavender-pm660-rtc.patch` | Skip XNU's 30 s wait for an `IORTC` resource (as XNU does for BCM2837) and feed the calendar from the PM660 RTC over SPMI (read-only; `sweetrtcoff=` when the raw count is not Unix time) | **On device**: the 30 s wait is gone (eMMC/USB at ~16 s instead of ~46 s); RTC read (raw `230219452`, not Unix time: offset measured in TWRP) |

| `0230-lavender-touch-spin-timing.patch` | Touch bit-bang I2C with real timing: `delay()` never spins on this port (CPUs registered with `powergate_latency` 0), so every half bit slept until a timer interrupt and a frame took seconds. Spin on the timebase (`sweetouchus=`), adaptive frame length/rate, timing self-test and 10 s stats | **On device: touch works** (home screen, icons), 0 I2C errors, ~14 ms idle read, up to ~65 ms full frame |
| `0231-lavender-touch-partial-frames.patch` | Read only up to the highest active slot (12 bytes for one finger instead of 65), idle poll 25 ms (`sweetouchidle=`), line-rise stats | Host syntax-checked; not yet run on the device |
| `0232-lavender-sdhci-dma-diag.patch` | Read-only SDHCI report at attach (CAPABILITIES, HOST_VERSION, clock/DLL/ADMA registers, bounce buffer address). Finding: this SDCC v5 core has **no SDMA** (CAPS bit 22), only ADMA2 (bit 19) with 64-bit addressing, which is why every SDMA attempt ended in `ADMAERR`. Optional timed tests behind `sweetemmc_speed=1` / `sweetemmc_dmatest=1` (off: they hold the controller lock and stall boot) | **On device** |
| `0233-lavender-sdhci-adma2-read.patch` | ADMA2 **read** path into the bounce buffer (32 KiB descriptors, cache maintenance, PIO fallback, off after 3 failures). Writes stay PIO (0192). On by default after a PIO-vs-ADMA2 self-test at two places of the root; `sweetemmc_dma=0` keeps PIO | **On device**: cold read 465 KB/s (PIO) -> ~16 MB/s; boot to the lock screen ~23 min -> ~2 min; dyld cache files hash-identical to the host under 4 concurrent readers |
| `0234-lavender-touch-real-panel-height.patch` | SweetHIDTouch takes the real panel size (1080x2340 from the boot video) instead of a hard-coded 2400: fixes the growing vertical offset and makes the bottom-edge band reachable (home/unlock swipe) | Pending device test |
| `0235-lavender-vsync-at-panel-rate.patch` | The vsync notification to CoreAnimation follows `sweetfb_hz` (every `fFlushTicks` ticks) instead of a fixed 60 Hz, so the CPU compositor (`CA::OGL::SW`, ~150 % of backboardd) no longer draws frames the panel never shows | Pending device test |
| `0236-lavender-gpu-probe.patch` | Adreno 512 bring-up ladder (`bsd/dev/lavender_gpu.c`), off unless `lavgpu=N`: 1 reads GCC/GPUCC state, 2 votes the GPU BIMC branches, 3 CX GDSC, 4 GX GDSC + core clocks + reads the RBBM/CP block. Each step is announced 2 s before the access. Project: `lavender-metal` | Pending device test |
| `0237-lavender-gpu-gx-root-clock.patch` | lavgpu step 4: force the GFX3D RCG root on (XO) before the GX GDSC, 3 s poll, collapse GX again on failure. Run `lavgpu=4` of 0236 left the GX GDSC at `08222000` (never PWR_ON): the DT marks it `force-enable-root-clk` | Pending device test |
| `0238-lavender-gpu-ldo-bhs.patch` | lavgpu: read-only dump of the GFX LDO/head-switch block (`0x0506e000`) in step 1; step 4 puts it in BHS mode under GDSC control before the GX GDSC (restored on failure). With 0237 alone the GX still stayed at `08222000` | Pending device test |
| `0239-lavender-gpu-gx-reset-clamp.patch` | lavgpu step 4: before clearing the GX GDSC's SW_COLLAPSE, pulse the GX block reset (GPUCC+0x1090) and the AON/GMEM reset and release the GMEM clamp (GPUCC+0x130), the order the vendor GDSC driver uses. 0237 and 0238 left the GX at `08222000`; the LDO block was already in BHS state (`pwrswitch_ctrl 80000038`) | Pending device test |
| `0240-lavender-gpu-scm-probe.patch` | lavgpu=5: SCM questions, no state change: INFO is_call_available(PIL IS_SUPPORTED) and PIL IS_SUPPORTED for PAS id 13 (GPU zap), 4 (modem, reference) and 99 (contrast). Same SMC64 convention as 0113. Runs after the GX power-up of 0239 (`lavgpu=4`: GX on at 11 ms, GPU block read, no hang) | Pending device test |
| `0241-lavender-gpu-zap-load.patch` | lavgpu=6: loads the GPU zap shader through TrustZone PIL (PAS 13): reads `a512_zap.mdt`/`.b02` from `/usr/share/firmware/lavender/gpu/`, INIT_IMAGE, MEM_SETUP, copy, AUTH_AND_RESET, each announced and stopped at the first non-zero ret; re-reads SECVID_TRUST_CNTL afterwards. Also logs `a530_pm4.fw` size | Device: INIT_IMAGE, MEM_SETUP and AUTH_AND_RESET all ret 0; SECVID_TRUST_CNTL stays 1 until the CP runs (expected) |
| `0243-lavender-gpu-smmu-probe.patch` | lavgpu=7: read-only look at the GPU kgsl SMMU (0x05040000, SMMUv2 layout): global sCR0/IDR*, stream mapping groups 0-3, context banks 0-3 (SCTLR, TTBR0, TCR, FSR, FAR). Nothing written. Runs after the zap load | Device: SMMU active (sCR0 00200626), S2CR0..3 = fault (reset default), CB0 disabled; reading CB1 hangs the reading thread, so 0244 reads CB0 and CB4 only |
| `0244-lavender-gpu-smmu-bypass.patch` | lavgpu=8: reads CB0 and CB4 only, then programs the GPU stream: SMR0 valid for ID 0 and S2CR0 type BYPASS (CP fetches by physical address), read back to confirm it stuck | Device: SMR0 took (80000000), but S2CR0 bypass came back as fault (0x20000): this SMMU refuses bypass, so 0245 translates |
| `0245-lavender-gpu-smmu-stage1.patch` | lavgpu=9: context bank 4 (gfx3d_user) gets a stage 1 AArch64 identity table (four 1 GB blocks, VA = PA, normal WB), TCR/MAIR/TTBR0/CBAR/CBA2R programmed, TLB invalidated, SCTLR enabled; stream 0 mapped to it (SMR0 valid ID 0, S2CR0 = CB4). Reads back each register | Device: all CB4 registers read back as written, SCTLR 67, stream 0 -> CB4, FSR 0 |
| `0246-lavender-gpu-ucode-ring.patch` | lavgpu=10: a530_pm4.fw / a530_pfp.fw (minus first dword) into contiguous buffers, CP_PFP/ME_INSTR_BASE, 32 KB ring + rptr shadow, CP_RB_CNTL/BASE/RPTR_ADDR written and read back. CP stays halted | Pending device test |

New lavender work takes the next free number from `0200` up.

## Changing the series

```bash
# after adding or editing a patch
python3 tools/extract_drivers.py      # refresh drivers/ mirrors (generated, never edited by hand)
bash tests/run_tests.sh               # patch structure, SDHCI/GPT/log/GIC host tests, shipped-code checks
bash tests/host/prepare-tree.sh /tmp/lavender-tree
bash tests/host/run.sh /tmp/lavender-tree   # USB/keys drivers on simulated registers
```

A push to `main` that touches `xnu/patches/**`, `resources/**` or the workflow builds the kernel in CI. `.gitattributes` keeps patches LF; a CRLF checkout makes `git apply` reject them.

## 0192: the 30 s vs 1 s timeout

The working patch uses `CACHE_FLUSH_TIMEOUT_NS` (**30 s**). A 1 s variant using `CMD6_BUSY_TIMEOUT_NS` was built as `0192b` (CI run `44636855966`, kernel `2a6b5421…`) and **rejected**: it stopped at `cc_fips_test` because some writes need more than one second. Do not shorten the timeout without a card-specific measurement.

## 0200: USB device

The base tree already had `sweet_usb.c` (patches `0072`–`0077`). It compiled for `LAVENDER`, but with the SM7150 addresses. It also never ran: its only caller is `sweet_ufs_init()`, and that function does not run on an eMMC phone.

- Addresses come from the phone's DTB (`tools/fdtdump.py` on `lavender_dtb.bin`): DWC3 core `0x0a800000`, Qualcomm wrapper (QSCRATCH) `0x0a8f8800`, QUSB2 **v1** PHY `0x0c012000`, GCC `0x00100000`. The GDSC and branch clock offsets come from Linux `gcc-sdm660.c` (`usb_30_gdsc` `0xf004`, master `0xf008`, sleep `0xf00c`, mock_utmi `0xf010`, cfg_noc_usb3_axi `0x5018`, aggre2_usb3_axi `0xf03c`, ahb2phy `0x6a004`, rx0_usb2_clkref `0x88018`).
- Boot-arg `sweetusb`:
  - `1` (default): read-only. DWC3 and the PHY are read only if the GCC says the block is powered and clocked.
  - `2`: starts the CDC-ECM gadget. The phone is `ue0` 192.168.7.2/24 and sends the console as UDP to 192.168.7.1:5140.
  - `3` (experimental): first turns on the USB30 GDSC and the seven branch clocks from the GCC, then does what `2` does. Lavender boots the shim from `recovery`, not with `fastboot boot`, so ABL may leave USB off. No RCG, reset, PMIC regulator or PHY tuning writes.
- If ABL left the core in host or OTG mode, it is put in device mode before the device soft reset.
- The live log reads the `santoni_sdhci.c` console ring through new readers, `santoni_log_copy()` and `santoni_log_position()`. The `sweet_ufs.c` ring stays empty on lavender.
- `sweet_keys_init()` and `sweet_usb_probe()` are called from `IOFindBSDRoot`, after the eMMC probe. Both are idempotent.

## 0201: buttons

`sweet_keys.c` was the topaz driver (SM6225, arbiter v5). From the DTB, SDM660 has a v2/v3 arbiter:

- core `0x0800f000`, observer `0x09400000`, cnfg `0x0800a000`, ee 0;
- the APID table is at core + `0x800`, there is no APID count register (512 APIDs), and each observer channel is at obsrvr + `0x8000 * apid`.

| Key | Source | Bit |
| --- | --- | --- |
| Power | PM660 (SID 0) PON `0x800`, KPDPWR | `PON_INT_RT_STS` bit 0 |
| Volume Down | PM660 PON, RESIN | `PON_INT_RT_STS` bit 1 |
| Volume Up | PM660L (SID 2) GPIO7 = `gpio@c600`, active low | `INT_RT_STS` (`0xc610`) bit 0 |

The driver is still read-only: the only MMIO write is the EXT_READL command. If the GPIO is not an enabled input, Volume Up is withheld and the PMIC is left unchanged. `sweetkeys=0` disables the driver. The `camera_*` entries in the DTB are reference-board leftovers (lavender has no camera key) and are not polled.

## Tests

- `tests/run_tests.sh` covers SDHCI (`0192`), GPT, the log ring, GIC, the payload tools, the workflow command lines and the patch structure.
- `tests/host/` compiles the real patched `sweet_usb.c`/`sweet_keys.c` against simulated registers.

See `tests/README.md`.

## 0191 note

The touch entry point is still a stub. It compiles as an unsupported-board stub so the kernel builds for lavender while the Novatek driver is missing. It is not a working touch driver.
