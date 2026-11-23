# NT36672A touch activation and boot-time evidence

Device investigation on 2026-11-23, using the supplied booting lavender shim
and TWRP 3.7.0_9-0.

## Replacement panel identification

Recovery touch works. Its bound input driver reports:

- input: `NVTCapacitiveTouchScreen`, `/dev/input/event2`;
- driver: `NVT-ts`, I2C device `1-0062` on `c175000.i2c`;
- coordinates: X `0..1079`, Y `0..2339`, ten slots;
- trim ID: `0a 00 00 72 66 03`;
- panel identification: `shenchao nt36672a fhdplus video mode dsi panel`;
- project ID `0x00f7`, PID `5916`.

This controller is in the family already handled by the kernel's Novatek
poller. The supplied shim lacked `sweetouchi2c=1`, and the captured iOS log
contained no `WHYRED TOUCH` probe/poll output. `santoni_touch_probe()` requires
that argument before it configures the bus or starts the poller.

`tools/build-shim.sh` now includes:

```
sweetouchi2c=1 sweetouchwait=0
```

The second argument removes the probe's default 15-second diagnostic hold.
Existing Android v0 images can be given these arguments with
`tools/repack-shim-cmdline.py`. Its tests verify the kernel/ADT bytes outside
the payload command-line field, appended DTB, ramdisk and second stage are
preserved and the Android component ID is recomputed.

## Captured boot performance

The supplied image already contains patch 0233's ADMA2 read path. In the
captured subsequent boot, both PIO-versus-ADMA2 tests returned `IDENTICAL`,
ADMA2 enabled at 14.661 seconds, and sustained reads were approximately
16,000–16,800 KiB/s. No ADMA2 failure/fallback was reported.

| Milestone | Uptime |
| --- | ---: |
| `exec launchd` | 15.306 s |
| `Early boot complete` | 25.149 s |
| `exec SpringBoard` | 25.766 s |
| `exec backboardd` | 26.464 s |
| First framebuffer swap | 66.694 s |
| First recorded SpringBoard surface creation | 170.714 s |

These are log milestones rather than a measurement of when the user first
sees an interactive lock screen. The user reported that later boots are
faster than the fresh rootfs's approximately 21-minute initial boot.

## Candidate status

The command-line-only candidate booted iOS but did not restore touch. Its
new probe output exposed a second issue:

```
[16.046] WHYRED TOUCH: ack 0x30
[16.111] WHYRED TOUCH: ack 0x62
[16.296] WHYRED TOUCH: nvt fwinfo err=0: ver=00/00 ... w=0 h=0
[16.296] WHYRED TOUCH: nvt fwinfo invalid
```

The old probe chose the first non-Synaptics ACK as `nvt_addr`, so it used
`0x30` rather than the confirmed Novatek endpoint. Patch
`0260-lavender-touch-novatek-address-selection.patch` restricts Novatek
selection to `0x01`/`0x62`, prefers firmware that is already running, and
skips Novatek register writes at other acknowledged addresses.

`tests/test_lavender_touch_probe.c` executes the actual patched probe with
the observed `0x30` then `0x62` sequence, firmware-only and mixed endpoint
cases, an unrelated ACK, a Synaptics ACK, and a disabled probe. The host
suite passes; the rebuilt kernel still requires on-device touch testing.
