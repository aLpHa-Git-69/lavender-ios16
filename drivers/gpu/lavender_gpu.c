/*
 * lavender: Adreno 512 bring-up ladder (M1). Boot-arg gated, off by default.
 *
 *   lavgpu=N        run the ladder up to step N (0 or absent: nothing happens)
 *   lavgpudelay=S   seconds to wait after boot before step 1 (default 25), so
 *                   the log flusher and the USB live log are up
 *
 * Steps, each one announced BEFORE the access, with a 2 s pause so the log
 * reaches the eMMC ring / USB (a wrong access on this SoC usually resets it
 * without a word):
 *   1  reads only: GCC branch state, and (if the GPU config AHB clock is on)
 *      the GPUCC block: PLLs, GDSCs, branches
 *   2  vote gcc_gpu_bimc_gfx_clk and gcc_bimc_gfx_clk, check the halt bits
 *   3  switch the CX GDSC on if it is off
 *   4  switch the GX GDSC on, release the GMEM clamp, enable the GFX3D core
 *      and RBBM timer branches (RCG left as the bootloader set it), then READ
 *      the RBBM / CP registers at 0x05000000
 * Nothing in this file writes a register inside the GPU block (0x05000000);
 * steps 2-4 write only GCC branch votes, GDSC controls and gpucc branches. No rail (CPR/RPM) vote is made:
 * step 4 is exactly the question "is the bootloader's rail state enough?".
 *
 * Offsets: Linux gcc-sdm660.c / gpucc-sdm660.c and Mesa a5xx.xml, read as
 * behaviour reference (values only, no code copied).
 */
#if defined(ARM64_BOARD_CONFIG_LAVENDER)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <arm/caches_internal.h>
#include <vm/memory_types.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <libkern/libkern.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define GCC_PA          0x00100000u
#define GCC_SIZE        0x80000u
#define GPUCC_PA        0x05065000u
#define GPUCC_SIZE      0x10000u
#define GPU_PA          0x05000000u
#define GPU_SIZE        0x40000u

/* GCC (byte offsets from GCC_PA) */
#define GCC_GPU_CFG_AHB         0x71004u
#define GCC_GPU_BIMC_GFX        0x71010u
#define GCC_BIMC_GFX            0x7106cu
#define GCC_GPLL0_VOTE          0x5200cu        /* bit 4 = gcc_gpu_gpll0_clk */

/* GPUCC (byte offsets from GPUCC_PA) */
#define CC_GPU_PLL0_MODE        0x0000u
#define CC_GPU_PLL1_MODE        0x0040u
#define CC_CX_GDSCR             0x1004u
#define CC_CX_GDS_HW_CTRL       0x1008u
#define CC_GX_GDSCR             0x1094u
#define CC_GX_CLAMP_IO          0x0130u
#define CC_GX_BCR               0x1090u
#define GX_GMEM_RESET           (1u << 4)
#define CC_GFX3D_CLK            0x1098u
#define CC_GFX3D_RCG_CMD        0x1070u
#define CC_GFX3D_RCG_CFG        0x1074u
#define CC_RBBMTIMER_CLK        0x10d0u
#define CC_RBBMTIMER_RCG_CMD    0x10b0u
#define CC_RBCPR_CLK            0x1054u
#define CC_RBCPR_RCG_CMD        0x1030u
#define CC_CX_GFX3D_CLK         0x1020u

/* GPU block (byte offsets; A5xx dword offsets from a5xx.xml times 4) */
#define A5_CP_RB_BASE           (0x0800u * 4)
#define A5_CP_RB_CNTL           (0x0802u * 4)
#define A5_RBBM_SW_RESET_CMD    (0x0043u * 4)
#define A5_RBBM_CLOCK_CNTL      (0x0067u * 4)
#define A5_RBBM_INT_0_STATUS    (0x04e1u * 4)
#define A5_RBBM_AHB_ERR_STATUS  (0x04f4u * 4)
#define A5_RBBM_STATUS          (0x04f5u * 4)
#define A5_RBBM_STATUS3         (0x0530u * 4)
#define A5_RBBM_SECVID_TRUST    (0xf400u * 4)

#define RCG_ROOT_EN     (1u << 1)
#define RCG_ROOT_OFF    (1u << 31)
/* msm_gfx_ldo block, inside the GPUCC window (0x0506e000 - 0x05065000) */
#define CC_LDO                  0x9000u
#define CC_LDO_PWRSWITCH_CTRL   (CC_LDO + 0x1cu)
#define CC_LDO_STATUS1          (CC_LDO + 0x24u)
#define CC_LDO_PWRSWITCH_STATUS (CC_LDO + 0x28u)
#define LDO_BHS_UNDER_SW        (1u << 0)
#define LDO_BHS_EN_FEW          (1u << 1)
#define LDO_BHS_EN_REST         (1u << 2)
#define LDO_UNDER_SW_CTRL       (1u << 4)
#define LDO_PWR_SRC_SEL         (1u << 9)
#define LDO_CPR_BYPASS          (1u << 30)

#define CBCR_OFF        (1u << 31)
#define CBCR_EN         (1u << 0)
#define GDSC_PWR_ON     (1u << 31)
#define GDSC_COLLAPSE   (1u << 0)

static vm_offset_t gcc, gpucc, gpu;
static int gx_on;
static int lav_chan;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
lav_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&lav_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
lav_announce(const char *what)
{
	printf("LAVGPU: about to %s\n", what);
	lav_wait(2000);
}

/* Poll a bit until it equals want; returns the last value. */
static uint32_t
lav_poll_n(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++) {
		v = rd(base, off);
		if ((v & mask) == want) {
			break;
		}
		lav_wait(10);
	}
	return v;
}

static uint32_t
lav_poll(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want)
{
	return lav_poll_n(base, off, mask, want, 100);
}

static void dump_ldo(const char *when);

static void
step1_reads(void)
{
	uint32_t ahb;

	lav_announce("step 1: read GCC GPU branches (read only)");
	ahb = rd(gcc, GCC_GPU_CFG_AHB);
	printf("LAVGPU: GCC cfg_ahb %08x bimc_gfx %08x gcc_bimc_gfx %08x "
	    "gpll0_vote %08x\n", ahb, rd(gcc, GCC_GPU_BIMC_GFX),
	    rd(gcc, GCC_BIMC_GFX), rd(gcc, GCC_GPLL0_VOTE));
	if (ahb & CBCR_OFF) {
		printf("LAVGPU: gcc_gpu_cfg_ahb_clk is OFF, GPUCC not read "
		    "(it would be an unclocked access)\n");
		return;
	}
	lav_announce("step 1: read GPUCC (AHB clock is on)");
	printf("LAVGPU: pll0 mode %08x pll1 mode %08x\n",
	    rd(gpucc, CC_GPU_PLL0_MODE), rd(gpucc, CC_GPU_PLL1_MODE));
	printf("LAVGPU: CX gdscr %08x hwctrl %08x GX gdscr %08x clamp %08x\n",
	    rd(gpucc, CC_CX_GDSCR), rd(gpucc, CC_CX_GDS_HW_CTRL),
	    rd(gpucc, CC_GX_GDSCR), rd(gpucc, CC_GX_CLAMP_IO));
	printf("LAVGPU: gfx3d clk %08x rcg cmd %08x cfg %08x; rbbmtimer clk "
	    "%08x rcg %08x; rbcpr clk %08x rcg %08x; cx_gfx3d %08x\n",
	    rd(gpucc, CC_GFX3D_CLK), rd(gpucc, CC_GFX3D_RCG_CMD),
	    rd(gpucc, CC_GFX3D_RCG_CFG), rd(gpucc, CC_RBBMTIMER_CLK),
	    rd(gpucc, CC_RBBMTIMER_RCG_CMD), rd(gpucc, CC_RBCPR_CLK),
	    rd(gpucc, CC_RBCPR_RCG_CMD), rd(gpucc, CC_CX_GFX3D_CLK));
	dump_ldo("step 1");
}

static void
vote_branch(const char *name, uint32_t off)
{
	uint32_t v = rd(gcc, off);
	printf("LAVGPU: %s before %08x\n", name, v);
	wr(gcc, off, v | CBCR_EN);
	v = lav_poll(gcc, off, CBCR_OFF, 0);
	printf("LAVGPU: %s after %08x (%s)\n", name, v,
	    (v & CBCR_OFF) ? "STILL OFF" : "on");
}

static void
step2_votes(void)
{
	lav_announce("step 2: vote gcc_gpu_bimc_gfx_clk and gcc_bimc_gfx_clk");
	vote_branch("gpu_bimc_gfx", GCC_GPU_BIMC_GFX);
	vote_branch("bimc_gfx", GCC_BIMC_GFX);
}

static int
gdsc_on_n(const char *name, uint32_t off, int polls)
{
	uint32_t v = rd(gpucc, off);

	if (v & GDSC_PWR_ON) {
		printf("LAVGPU: %s GDSC already on (%08x)\n", name, v);
		return 0;
	}
	printf("LAVGPU: %s GDSC off (%08x), clearing SW_COLLAPSE\n", name, v);
	wr(gpucc, off, v & ~GDSC_COLLAPSE);
	v = lav_poll_n(gpucc, off, GDSC_PWR_ON, GDSC_PWR_ON, polls);
	printf("LAVGPU: %s GDSC now %08x (%s)\n", name, v,
	    (v & GDSC_PWR_ON) ? "on" : "STILL OFF");
	return (v & GDSC_PWR_ON) ? 0 : EIO;
}

static int
gdsc_on(const char *name, uint32_t off)
{
	return gdsc_on_n(name, off, 100);
}

static int
step3_cx(void)
{
	if (rd(gcc, GCC_GPU_CFG_AHB) & CBCR_OFF) {
		return EIO;
	}
	lav_announce("step 3: CX GDSC on");
	return gdsc_on("CX", CC_CX_GDSCR);
}

static void
dump_gpu(void)
{
	printf("LAVGPU: RBBM_STATUS %08x STATUS3 %08x INT_0 %08x AHB_ERR %08x\n",
	    rd(gpu, A5_RBBM_STATUS), rd(gpu, A5_RBBM_STATUS3),
	    rd(gpu, A5_RBBM_INT_0_STATUS), rd(gpu, A5_RBBM_AHB_ERR_STATUS));
	printf("LAVGPU: SECVID_TRUST_CNTL %08x CLOCK_CNTL %08x SW_RESET %08x "
	    "CP_RB_BASE %08x CP_RB_CNTL %08x\n", rd(gpu, A5_RBBM_SECVID_TRUST),
	    rd(gpu, A5_RBBM_CLOCK_CNTL), rd(gpu, A5_RBBM_SW_RESET_CMD),
	    rd(gpu, A5_CP_RB_BASE), rd(gpu, A5_CP_RB_CNTL));
}

static void
dump_ldo(const char *when)
{
	printf("LAVGPU: gfx ldo (%s): atest %08x cfg0 %08x cfg1 %08x cfg2 %08x "
	    "ld %08x vreftest %08x vref %08x\n", when, rd(gpucc, CC_LDO),
	    rd(gpucc, CC_LDO + 0x4), rd(gpucc, CC_LDO + 0x8),
	    rd(gpucc, CC_LDO + 0xc), rd(gpucc, CC_LDO + 0x10),
	    rd(gpucc, CC_LDO + 0x14), rd(gpucc, CC_LDO + 0x18));
	printf("LAVGPU: gfx ldo (%s): pwrswitch_ctrl %08x status1 %08x "
	    "pwrswitch_status %08x\n", when, rd(gpucc, CC_LDO_PWRSWITCH_CTRL),
	    rd(gpucc, CC_LDO_STATUS1), rd(gpucc, CC_LDO_PWRSWITCH_STATUS));
}

/*
 * The GX power domain is fed through the GFX LDO/head-switch block. Put it in
 * BHS mode under GDSC control, the same bit sequence the vendor regulator
 * runs on enable (msm_gfx_ldo.c: enable_bhs_mode, behaviour reference only).
 */
static void
ldo_bhs_mode(void)
{
	uint32_t ctl = rd(gpucc, CC_LDO_PWRSWITCH_CTRL);

	ctl |= LDO_UNDER_SW_CTRL;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_PWR_SRC_SEL;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_CPR_BYPASS;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_BHS_EN_FEW;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_BHS_EN_REST;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_BHS_UNDER_SW;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
}

static void
step4_gx(void)
{
	uint32_t v, ldo_ctl_saved;

	/*
	 * Run 3 (0236): the GX GDSC left the off state (GDSCR 08222000) but never
	 * reported PWR_ON. The DT marks it qcom,force-enable-root-clk with
	 * core_root_clk = the GFX3D RCG, which was still ROOT_OFF. Turn the root on
	 * first; its source is XO (cfg 0, 19.2 MHz), no PLL and no rail involved.
	 */
	lav_announce("step 4: force the GFX3D RCG root on (XO source)");
	v = rd(gpucc, CC_GFX3D_RCG_CMD);
	printf("LAVGPU: gfx3d rcg cmd before %08x cfg %08x\n", v,
	    rd(gpucc, CC_GFX3D_RCG_CFG));
	wr(gpucc, CC_GFX3D_RCG_CMD, v | RCG_ROOT_EN);
	v = lav_poll(gpucc, CC_GFX3D_RCG_CMD, RCG_ROOT_OFF, 0);
	printf("LAVGPU: gfx3d rcg cmd after %08x (%s)\n", v,
	    (v & RCG_ROOT_OFF) ? "root STILL OFF" : "root on");
	dump_ldo("before");
	lav_announce("step 4: put the GFX LDO block in BHS mode under GDSC control");
	ldo_ctl_saved = rd(gpucc, CC_LDO_PWRSWITCH_CTRL);
	ldo_bhs_mode();
	dump_ldo("after bhs");
	/*
	 * Run 5 (0238): the LDO bits were already in BHS state and the GX GDSC
	 * still stayed at 08222000. The vendor GDSC driver does three more things
	 * BEFORE it clears SW_COLLAPSE (gdsc-regulator.c, behaviour reference):
	 * pulse the GX block reset (sw-reset = GPUCC+0x1090 bit 0), pulse the
	 * AON/GMEM reset (domain-addr = GPUCC+0x130 bit 4, reset-aon-logic) and
	 * release the GMEM clamp (same register, bit 0). Until now the clamp was
	 * only released after the power-up that never completed.
	 */
	lav_announce("step 4: GX block reset pulse, AON reset pulse, release clamp");
	v = rd(gpucc, CC_GX_BCR);
	printf("LAVGPU: GX bcr %08x clamp/domain %08x\n", v,
	    rd(gpucc, CC_GX_CLAMP_IO));
	wr(gpucc, CC_GX_BCR, v | 1u);
	for (int i = 0; i < 20; i++) {
		(void)rd(gpucc, CC_GX_GDSCR);
	}
	wr(gpucc, CC_GX_BCR, v & ~1u);
	v = rd(gpucc, CC_GX_CLAMP_IO);
	wr(gpucc, CC_GX_CLAMP_IO, v | GX_GMEM_RESET);
	for (int i = 0; i < 20; i++) {
		(void)rd(gpucc, CC_GX_GDSCR);
	}
	wr(gpucc, CC_GX_CLAMP_IO, v & ~GX_GMEM_RESET);
	wr(gpucc, CC_GX_CLAMP_IO, rd(gpucc, CC_GX_CLAMP_IO) & ~1u);
	printf("LAVGPU: GX bcr %08x clamp/domain %08x\n", rd(gpucc, CC_GX_BCR),
	    rd(gpucc, CC_GX_CLAMP_IO));
	lav_announce("step 4: GX GDSC on");
	if (gdsc_on_n("GX", CC_GX_GDSCR, 300)) {
		lav_wait(1000);
		printf("LAVGPU: GX after 1 more s: gdscr %08x cfg(0x1098) %08x\n",
		    rd(gpucc, CC_GX_GDSCR), rd(gpucc, CC_GFX3D_CLK));
		/* leave the domain as found: collapse it, drop the forced root,
		 * restore the LDO block */
		wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ldo_ctl_saved);
		wr(gpucc, CC_GX_CLAMP_IO, rd(gpucc, CC_GX_CLAMP_IO) | 1u);
		wr(gpucc, CC_GX_GDSCR, rd(gpucc, CC_GX_GDSCR) | GDSC_COLLAPSE);
		wr(gpucc, CC_GFX3D_RCG_CMD,
		    rd(gpucc, CC_GFX3D_RCG_CMD) & ~RCG_ROOT_EN);
		lav_wait(100);
		printf("LAVGPU: GX collapsed again: gdscr %08x rcg %08x\n",
		    rd(gpucc, CC_GX_GDSCR), rd(gpucc, CC_GFX3D_RCG_CMD));
		return;
	}
	v = rd(gpucc, CC_GX_CLAMP_IO);
	printf("LAVGPU: GMEM clamp before %08x, clearing bit 0\n", v);
	wr(gpucc, CC_GX_CLAMP_IO, v & ~1u);
	lav_announce("step 4: enable gfx3d and rbbmtimer branches");
	for (int i = 0; i < 2; i++) {
		uint32_t off = i ? CC_RBBMTIMER_CLK : CC_GFX3D_CLK;
		v = rd(gpucc, off);
		wr(gpucc, off, v | CBCR_EN);
		v = lav_poll(gpucc, off, CBCR_OFF, 0);
		printf("LAVGPU: %s clk %08x (%s)\n", i ? "rbbmtimer" : "gfx3d",
		    v, (v & CBCR_OFF) ? "STILL OFF" : "on");
	}
	lav_announce("step 4: READ the GPU block at 0x05000000");
	dump_gpu();
	gx_on = 1;
	printf("LAVGPU: step 4 done, the GPU block answered\n");
}

/*
 * Step 5: does the SDM660 TrustZone take our SCM call, and does it know the
 * GPU zap shader (PAS id 13)? Questions only, no state change. The calling
 * convention is the one 0113 uses for the modem (SMC64, owner SIP, yielding
 * call, resumed while x0 == 1); Linux qcom_scm-smc.c as behaviour reference.
 */
#define SCM_FN(svc, cmd)        (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_INFO            0x06
#define SCM_INFO_IS_CALL_AVAIL  0x01
#define SCM_SVC_PIL             0x02
#define PIL_IS_SUPPORTED        0x07

static uint64_t
lav_scm(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c,
    uint64_t *res1)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo,
	    x2 __asm__("x2") = a, x3 __asm__("x3") = b, x4 __asm__("x4") = c,
	    x5 __asm__("x5") = 0, x6 __asm__("x6") = 0, x7 __asm__("x7") = 0;

	for (int n = 0; n < 1000; n++) {
		__asm__ volatile ("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
		      "+r"(x5), "+r"(x6), "+r"(x7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		      "x16", "x17", "memory");
		if (x0 != 1) {
			break;
		}
		/* INTERRUPTED: resume with the session id the secure side gave */
		x0 = 1;
		x1 = arginfo;
		x2 = a;
		x3 = b;
		x4 = c;
		x5 = 0;
		x7 = 0;
	}
	*res1 = x1;
	return x0;
}

static void
step5_scm(void)
{
	static const uint32_t ids[] = { 13, 4, 99 };
	static const char *const names[] = { "gpu zap", "modem (reference)",
	    "unused (contrast)" };
	uint64_t ret, res = 0;

	lav_announce("step 5: SCM INFO is_call_available(PIL IS_SUPPORTED)");
	ret = lav_scm(SCM_FN(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL), 1,
	    SCM_FN(SCM_SVC_PIL, PIL_IS_SUPPORTED), 0, 0, &res);
	printf("LAVGPU: SCM is_call_available: ret %llx res %llx\n",
	    (unsigned long long)ret, (unsigned long long)res);
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		lav_announce("step 5: SCM PIL IS_SUPPORTED for the next id");
		ret = lav_scm(SCM_FN(SCM_SVC_PIL, PIL_IS_SUPPORTED), 1, ids[i], 0, 0,
		    &res);
		printf("LAVGPU: SCM PAS id %u (%s) supported: ret %llx res %llx\n",
		    ids[i], names[i], (unsigned long long)ret,
		    (unsigned long long)res);
	}
}

/*
 * Step 6: load the GPU zap shader through TrustZone. Same sequence the vendor
 * PIL driver runs (Linux qcom_mdt_load + qcom_scm_pas_*, behaviour reference):
 *   INIT_IMAGE   metadata = a512_zap.mdt (ELF header + hash segment), in a
 *                physically contiguous non-secure buffer
 *   MEM_SETUP    the region the image will be placed in (relocatable image)
 *   copy         the PT_LOAD segment (a512_zap.b02) into that region
 *   AUTH_AND_RESET
 * After AUTH_AND_RESET the region belongs to the secure side: it is kept
 * allocated (permanent) and never touched again.
 */
#define GPU_FW_DIR              "/usr/share/firmware/lavender/gpu/"
#define PAS_ID_GPU_ZAP          13
#define PIL_INIT_IMAGE          0x01
#define PIL_MEM_SETUP           0x02
#define PIL_AUTH_RESET          0x05
#define SCM_ARG_RW              2
#define ZAP_META_MAX            0x2000u
#define ZAP_CODE_MAX            0x10000u

struct lav_elf32_ehdr {
	uint8_t  e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct lav_elf32_phdr {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags,
	    p_align;
};

static int
lav_alloc(vm_size_t size, vm_offset_t mask, vm_offset_t *va, uint64_t *pa)
{
	ppnum_t pn;

	*va = 0;
	if (kmem_alloc_contig(kernel_map, va, size, mask, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		return ENOMEM;
	}
	pn = pmap_find_phys(kernel_pmap, (addr64_t)*va);
	if (pn == 0) {
		return ENOMEM;
	}
	*pa = ptoa_64(pn);
	return 0;
}

/* Read a whole file (at most max bytes) into buf; *len gets its size. */
static int
lav_read_file(const char *name, void *buf, uint32_t max, uint32_t *len)
{
	char path[128];
	vnode_t vp = NULLVP;
	vfs_context_t ctx = vfs_context_kernel();
	int err, resid = 0;

	snprintf(path, sizeof(path), "%s%s", GPU_FW_DIR, name);
	err = vnode_open(path, FREAD, 0, 0, &vp, ctx);
	if (err) {
		printf("LAVGPU: open %s failed: %d\n", path, err);
		return err;
	}
	err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)max, 0, UIO_SYSSPACE,
	    IO_NODELOCKED, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	(void)vnode_close(vp, FREAD, ctx);
	if (err) {
		printf("LAVGPU: read %s failed: %d\n", path, err);
		return err;
	}
	*len = max - (uint32_t)resid;
	return 0;
}

static void
step6_zap(void)
{
	vm_offset_t meta = 0, code = 0, region = 0;
	uint64_t meta_pa = 0, code_pa = 0, region_pa = 0, ret, res = 0;
	uint32_t meta_len = 0, code_len = 0, fw_len = 0;
	const struct lav_elf32_ehdr *eh;
	const struct lav_elf32_phdr *ph, *load = NULL;
	uint32_t min_addr, max_addr, size;

	lav_announce("step 6: read the zap firmware from the root filesystem");
	if (lav_alloc(ZAP_META_MAX, PAGE_MASK, &meta, &meta_pa) ||
	    lav_alloc(ZAP_CODE_MAX, PAGE_MASK, &code, &code_pa)) {
		printf("LAVGPU: no contiguous memory for the zap image\n");
		return;
	}
	if (lav_read_file("a512_zap.mdt", (void *)meta, ZAP_META_MAX, &meta_len) ||
	    lav_read_file("a512_zap.b02", (void *)code, ZAP_CODE_MAX, &code_len)) {
		return;
	}
	/* The microcode is only checked and counted here; it is loaded later. */
	if (lav_read_file("a530_pm4.fw", (void *)(code + 0x8000), 0x8000,
	    &fw_len) == 0) {
		printf("LAVGPU: a530_pm4.fw %u bytes\n", fw_len);
	}
	printf("LAVGPU: a512_zap.mdt %u bytes, a512_zap.b02 %u bytes, "
	    "meta pa %llx\n", meta_len, code_len, (unsigned long long)meta_pa);
	eh = (const struct lav_elf32_ehdr *)meta;
	if (meta_len < sizeof(*eh) || memcmp(eh->e_ident, "\177ELF", 4) != 0 ||
	    eh->e_phentsize != sizeof(*ph) || eh->e_phnum == 0 ||
	    eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(*ph) > meta_len) {
		printf("LAVGPU: a512_zap.mdt is not the expected ELF\n");
		return;
	}
	ph = (const struct lav_elf32_phdr *)(meta + eh->e_phoff);
	min_addr = 0xffffffffu;
	max_addr = 0;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != 1 || ph[i].p_memsz == 0) {
			continue;
		}
		if (ph[i].p_paddr < min_addr) {
			min_addr = ph[i].p_paddr;
		}
		if (ph[i].p_paddr + ph[i].p_memsz > max_addr) {
			max_addr = ph[i].p_paddr + ph[i].p_memsz;
		}
		load = &ph[i];
	}
	if (load == NULL || load->p_filesz != code_len ||
	    load->p_memsz < load->p_filesz) {
		printf("LAVGPU: zap segment does not match a512_zap.b02\n");
		return;
	}
	min_addr &= ~0xfffu;
	max_addr = (max_addr + 0xfffu) & ~0xfffu;
	size = max_addr - min_addr;
	printf("LAVGPU: zap region 0x%x..0x%x (%u bytes), load offset 0x%x\n",
	    min_addr, max_addr, size, load->p_paddr - min_addr);
	if (size > 0x100000 ||
	    (lav_alloc(size, 0xfffff, &region, &region_pa) &&
	    lav_alloc(size, PAGE_MASK, &region, &region_pa))) {
		printf("LAVGPU: no contiguous region for the zap\n");
		return;
	}
	printf("LAVGPU: zap region pa %llx, SECVID_TRUST_CNTL before %08x\n",
	    (unsigned long long)region_pa, gx_on ? rd(gpu, A5_RBBM_SECVID_TRUST) : 0);

	flush_dcache64((addr64_t)meta_pa, meta_len, TRUE);
	lav_announce("step 6: SCM PIL INIT_IMAGE (metadata)");
	ret = lav_scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE),
	    2 | (SCM_ARG_RW << 6), PAS_ID_GPU_ZAP, meta_pa, 0, &res);
	printf("LAVGPU: init_image: ret %llx res %llx\n", (unsigned long long)ret,
	    (unsigned long long)res);
	if (ret != 0) {
		return;
	}
	lav_announce("step 6: SCM PIL MEM_SETUP (region)");
	ret = lav_scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, PAS_ID_GPU_ZAP,
	    region_pa, size, &res);
	printf("LAVGPU: mem_setup: ret %llx res %llx\n", (unsigned long long)ret,
	    (unsigned long long)res);
	if (ret != 0) {
		return;
	}
	memcpy((void *)(region + (load->p_paddr - min_addr)), (void *)code,
	    code_len);
	flush_dcache64((addr64_t)region_pa, size, TRUE);
	lav_announce("step 6: SCM PIL AUTH_AND_RESET (loads the zap)");
	ret = lav_scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, PAS_ID_GPU_ZAP, 0,
	    0, &res);
	printf("LAVGPU: auth_and_reset: ret %llx res %llx\n",
	    (unsigned long long)ret, (unsigned long long)res);
	lav_wait(100);
	if (gx_on) {
		printf("LAVGPU: after the zap: RBBM_STATUS %08x SECVID_TRUST_CNTL "
		    "%08x\n", rd(gpu, A5_RBBM_STATUS), rd(gpu, A5_RBBM_SECVID_TRUST));
	}
}

/*
 * Step 7: read-only look at the GPU's own SMMU (kgsl-smmu-v2, ARM SMMUv2
 * register layout). The CP fetches microcode and the ring through it, so we
 * need to know whether it translates, is in bypass, or is locked by the
 * secure side. Nothing is written.
 */
#define SMMU_PA                 0x05040000u
#define SMMU_SIZE               0x10000u

static vm_offset_t smmu_va;

static void
step7_smmu(void)
{
	vm_offset_t smmu;
	uint32_t idr0, idr1, idr2, idr7, pgsz, npage, cbbase;

	smmu = ml_io_map(SMMU_PA, SMMU_SIZE);
	if (smmu == 0) {
		printf("LAVGPU: SMMU map failed\n");
		return;
	}
	lav_announce("step 7: READ the SMMU global registers");
	idr0 = rd(smmu, 0x20);
	idr1 = rd(smmu, 0x24);
	idr2 = rd(smmu, 0x28);
	idr7 = rd(smmu, 0x3c);
	printf("LAVGPU: SMMU sCR0 %08x sCR1 %08x sCR2 %08x sACR %08x\n",
	    rd(smmu, 0x0), rd(smmu, 0x4), rd(smmu, 0x8), rd(smmu, 0x10));
	printf("LAVGPU: SMMU IDR0 %08x IDR1 %08x IDR2 %08x IDR7 %08x\n", idr0,
	    idr1, idr2, idr7);
	pgsz = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	npage = 1u << (((idr1 >> 28) & 7) + 1);
	cbbase = (npage * pgsz) / 2;
	printf("LAVGPU: SMMU page size %x, %u pages, %u context banks, "
	    "%u stream-mapping groups, CB base +%x\n", pgsz, npage,
	    idr1 & 0xff, (idr0 >> 0) & 0xff, cbbase);

	lav_announce("step 7: READ stream mapping groups 0..3");
	for (uint32_t i = 0; i < 4; i++) {
		printf("LAVGPU: SMMU SMR%u %08x S2CR%u %08x\n", i,
		    rd(smmu, 0x800 + 4 * i), i, rd(smmu, 0xc00 + 4 * i));
	}
	if (cbbase + 4 * pgsz > SMMU_SIZE) {
		printf("LAVGPU: context banks are outside the mapped block\n");
		return;
	}
	lav_announce("step 7: READ context banks 0 and 4 (1..3 hang the reader)");
	for (uint32_t cb = 0; cb < 5; cb += 4) {
		vm_offset_t b = smmu + cbbase + cb * pgsz;
		printf("LAVGPU: CB%u SCTLR %08x TTBR0 %08x%08x TCR %08x FSR %08x "
		    "FAR %08x\n", cb, rd(b, 0x0), rd(b, 0x24), rd(b, 0x20),
		    rd(b, 0x30), rd(b, 0x58), rd(b, 0x60));
	}
	smmu_va = smmu;
}

/*
 * Step 8: let the GPU's stream (SID 0) through the SMMU untranslated, so the
 * CP can fetch microcode and the ring by physical address (S2CR type BYPASS).
 * SMR0 matches only ID 0; no other master uses this SMMU's stream 0.
 */
static void
step8_smmu_bypass(void)
{
	uint32_t smr, s2cr;

	if (smmu_va == 0) {
		return;
	}
	lav_announce("step 8: WRITE SMR0 (valid, ID 0) and S2CR0 (bypass)");
	wr(smmu_va, 0x800, (1u << 31) | 0u);
	wr(smmu_va, 0xc00, (1u << 16) | 0u);
	smr = rd(smmu_va, 0x800);
	s2cr = rd(smmu_va, 0xc00);
	printf("LAVGPU: SMMU SMR0 now %08x S2CR0 now %08x (%s)\n", smr, s2cr,
	    (smr == ((1u << 31)) && ((s2cr >> 16) & 3) == 1) ? "bypass stuck" :
	    "did not take");
}

/*
 * Step 9: the SMMU turned the bypass request into "fault", so translate: give
 * context bank 4 (gfx3d_user, DT gpu-offset 0x48000) a stage 1 AArch64 table
 * that maps the first 4 GB 1:1 (four 1 GB blocks, normal write-back memory),
 * and point stream 0 at it. The CP then fetches microcode and the ring by
 * physical address through the table. Register layout: ARM SMMUv2.
 */
#define SMMU_CB                 4u
#define SMMU_CBAR(cb)           (0x1000u + 4u * (cb))
#define SMMU_CBA2R(cb)          (0x8000u + 4u * (cb))
#define CB_SCTLR                0x00u
#define CB_TCR2                 0x10u
#define CB_TTBR0                0x20u
#define CB_TCR                  0x30u
#define CB_MAIR0                0x38u
#define CB_FSR                  0x58u
#define CB_TLBIALL              0x618u
#define CB_TLBSYNC              0x7f0u
#define CB_TLBSTATUS            0x7f4u

static uint64_t smmu_pt_pa;

static void
step9_smmu_s1(void)
{
	vm_offset_t pt = 0, cb;
	uint64_t pa = 0;
	uint32_t pgsz = 0x1000, cbbase = 0x4000;

	if (smmu_va == 0) {
		return;
	}
	if (lav_alloc(0x1000, PAGE_MASK, &pt, &pa)) {
		printf("LAVGPU: no page for the SMMU table\n");
		return;
	}
	for (unsigned i = 0; i < 4; i++) {
		/* 1 GB block: VA = PA, AF, inner shareable, attr index 0, valid */
		((volatile uint64_t *)pt)[i] = ((uint64_t)i << 30) | (1ull << 10) |
		    (3ull << 8) | 1ull;
	}
	flush_dcache64((addr64_t)pa, 0x1000, TRUE);
	smmu_pt_pa = pa;
	cb = smmu_va + cbbase + SMMU_CB * pgsz;

	lav_announce("step 9: WRITE context bank 4 (stage 1, identity table)");
	wr(smmu_va, SMMU_CBA2R(SMMU_CB), 1u);                  /* VA64 */
	wr(smmu_va, SMMU_CBAR(SMMU_CB), 1u << 16);             /* S1 translate, S2 bypass */
	wr(cb, CB_TCR2, 0x1u);                                 /* 36-bit output */
	wr(cb, CB_TCR, 25u | (1u << 8) | (1u << 10) | (3u << 12)); /* 39-bit VA, 4K, WB, inner */
	wr(cb, CB_MAIR0, 0xffu);
	wr(cb, CB_TTBR0, (uint32_t)pa);
	wr(cb, CB_TTBR0 + 4, (uint32_t)(pa >> 32));
	wr(cb, CB_FSR, 0xffffffffu);
	wr(cb, CB_TLBIALL, 0);
	wr(cb, CB_TLBSYNC, 0);
	for (int i = 0; i < 1000 && (rd(cb, CB_TLBSTATUS) & 1); i++) {
		lav_wait(1);
	}
	printf("LAVGPU: CB4 before enable: CBAR %08x CBA2R %08x TCR %08x TCR2 %08x "
	    "TTBR0 %08x%08x MAIR0 %08x\n", rd(smmu_va, SMMU_CBAR(SMMU_CB)),
	    rd(smmu_va, SMMU_CBA2R(SMMU_CB)), rd(cb, CB_TCR), rd(cb, CB_TCR2),
	    rd(cb, CB_TTBR0 + 4), rd(cb, CB_TTBR0), rd(cb, CB_MAIR0));

	lav_announce("step 9: enable context bank 4 and map stream 0 to it");
	/* M, TRE, AFE, CFRE, CFIE: translate, report faults, no stalling */
	wr(cb, CB_SCTLR, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6));
	wr(smmu_va, 0xc00, SMMU_CB);                           /* S2CR0: translate via CB4 */
	wr(smmu_va, 0x800, 1u << 31);                          /* SMR0: valid, ID 0 */
	printf("LAVGPU: CB4 SCTLR %08x FSR %08x; SMR0 %08x S2CR0 %08x (%s)\n",
	    rd(cb, CB_SCTLR), rd(cb, CB_FSR), rd(smmu_va, 0x800),
	    rd(smmu_va, 0xc00),
	    (rd(smmu_va, 0xc00) & 0x3ffffu) == SMMU_CB ? "stream 0 -> CB4" :
	    "did not take");
}

/*
 * Step 10: microcode and ring, CP still halted. Put the PM4 (ME) and PFP
 * images in physically contiguous buffers (the firmware file minus its first
 * dword, as the vendor driver does), point the CP at them, allocate the ring
 * and the read-pointer shadow, program the ring registers, read everything
 * back. The CP is not started here. All addresses are PA = GPU VA through the
 * identity table of step 9.
 */
#define A5_CP_RB_BASE           (0x0800u * 4)
#define A5_CP_RB_CNTL           (0x0802u * 4)
#define A5_CP_RB_RPTR_ADDR      (0x0804u * 4)
#define A5_CP_RB_RPTR           (0x0806u * 4)
#define A5_CP_RB_WPTR           (0x0807u * 4)
#define A5_CP_CNTL              (0x0831u * 4)
#define A5_CP_PFP_ME_CNTL       (0x0832u * 4)
#define A5_CP_PFP_INSTR_BASE    (0x0835u * 4)
#define A5_CP_ME_INSTR_BASE     (0x0838u * 4)
#define RING_BYTES              0x8000u
#define UCODE_BYTES             0x5000u

static uint64_t ring_pa, rptr_pa, fence_pa;
static vm_offset_t ring_va, rptr_va;

static int
lav_load_ucode(const char *name, uint64_t *pa)
{
	vm_offset_t va = 0;
	uint32_t len = 0;

	if (lav_alloc(UCODE_BYTES, PAGE_MASK, &va, pa) ||
	    lav_read_file(name, (void *)va, UCODE_BYTES, &len)) {
		return -1;
	}
	if (len < 8 || len > UCODE_BYTES) {
		printf("LAVGPU: %s has an unexpected size %u\n", name, len);
		return -1;
	}
	memmove((void *)va, (void *)(va + 4), len - 4);
	flush_dcache64((addr64_t)*pa, UCODE_BYTES, TRUE);
	printf("LAVGPU: %s: %u bytes of microcode at pa %llx\n", name, len - 4,
	    (unsigned long long)*pa);
	return 0;
}

static void
step10_ucode_ring(void)
{
	uint64_t pm4_pa, pfp_pa;

	if (!gx_on || smmu_pt_pa == 0) {
		printf("LAVGPU: step 10 needs the GX power-up and the SMMU table\n");
		return;
	}
	lav_announce("step 10: read the microcode files");
	if (lav_load_ucode("a530_pm4.fw", &pm4_pa) ||
	    lav_load_ucode("a530_pfp.fw", &pfp_pa) ||
	    lav_alloc(RING_BYTES, PAGE_MASK, &ring_va, &ring_pa) ||
	    lav_alloc(0x1000, PAGE_MASK, &rptr_va, &rptr_pa)) {
		printf("LAVGPU: step 10: buffers not available\n");
		return;
	}
	fence_pa = rptr_pa + 0x100;
	printf("LAVGPU: ring pa %llx, rptr shadow pa %llx, fence pa %llx\n",
	    (unsigned long long)ring_pa, (unsigned long long)rptr_pa,
	    (unsigned long long)fence_pa);
	printf("LAVGPU: CP_CNTL %08x CP_PFP_ME_CNTL %08x before\n",
	    rd(gpu, A5_CP_CNTL), rd(gpu, A5_CP_PFP_ME_CNTL));

	lav_announce("step 10: WRITE CP microcode base and ring registers");
	wr(gpu, A5_CP_PFP_INSTR_BASE, (uint32_t)pfp_pa);
	wr(gpu, A5_CP_PFP_INSTR_BASE + 4, (uint32_t)(pfp_pa >> 32));
	wr(gpu, A5_CP_ME_INSTR_BASE, (uint32_t)pm4_pa);
	wr(gpu, A5_CP_ME_INSTR_BASE + 4, (uint32_t)(pm4_pa >> 32));
	/* ring: BUFSZ = log2(bytes/8) in bits 5:0, BLKSZ = log2(32/8) in bits 13:8 */
	wr(gpu, A5_CP_RB_CNTL, 12u | (2u << 8));
	wr(gpu, A5_CP_RB_BASE, (uint32_t)ring_pa);
	wr(gpu, A5_CP_RB_BASE + 4, (uint32_t)(ring_pa >> 32));
	wr(gpu, A5_CP_RB_RPTR_ADDR, (uint32_t)rptr_pa);
	wr(gpu, A5_CP_RB_RPTR_ADDR + 4, (uint32_t)(rptr_pa >> 32));
	printf("LAVGPU: PFP base %08x%08x ME base %08x%08x\n",
	    rd(gpu, A5_CP_PFP_INSTR_BASE + 4), rd(gpu, A5_CP_PFP_INSTR_BASE),
	    rd(gpu, A5_CP_ME_INSTR_BASE + 4), rd(gpu, A5_CP_ME_INSTR_BASE));
	printf("LAVGPU: RB_CNTL %08x RB_BASE %08x%08x RPTR_ADDR %08x%08x RPTR %08x "
	    "WPTR %08x\n", rd(gpu, A5_CP_RB_CNTL), rd(gpu, A5_CP_RB_BASE + 4),
	    rd(gpu, A5_CP_RB_BASE), rd(gpu, A5_CP_RB_RPTR_ADDR + 4),
	    rd(gpu, A5_CP_RB_RPTR_ADDR), rd(gpu, A5_CP_RB_RPTR),
	    rd(gpu, A5_CP_RB_WPTR));
}

/*
 * Step 11: start the command processor. Behaviour reference: Linux a5xx
 * hw_init / me_init / flush (no code taken). Minimal subset: 64-bit address
 * modes, secure-video block off, CP queue thresholds, un-halt PFP/ME, then
 * packets through the ring: CP_ME_INIT, then CP_SET_SECURE_MODE(0), then a
 * CP_MEM_WRITE to a fence word that the CPU reads back. Every submission is
 * announced and the ladder stops at the first one the CP does not consume.
 */
#define A5_RBBM_INT_0_MASK      (0x0038u * 4)
#define A5_CP_ROQ_THRESH_1      (0x081fu * 4)
#define A5_CP_ROQ_THRESH_2      (0x0820u * 4)
#define A5_CP_MEQ_THRESHOLDS    (0x0825u * 4)
#define A5_CP_MERCIU_SIZE       (0x0826u * 4)
#define A5_CP_CHICKEN_DBG       (0x0833u * 4)
#define A5_CP_ADDR_MODE_CNTL    (0x0860u * 4)
#define A5_SECVID_TSB_BASE_LO   (0xf800u * 4)
#define A5_SECVID_TSB_SIZE      (0xf802u * 4)
#define A5_SECVID_TSB_CNTL      (0xf803u * 4)
#define A5_SECVID_TSB_ADDR_MODE (0xf810u * 4)
#define RB_NO_UPDATE            (1u << 27)

#define CP_NOP                  16u
#define CP_ME_INIT              72u
#define CP_MEM_WRITE            61u
#define CP_SET_SECURE_MODE      102u

static uint32_t ring_wptr;

/* Odd parity bit of val, as the PM4 type 7 header wants it. */
static uint32_t
pm4_parity(uint32_t val)
{
	val ^= val >> 16;
	val ^= val >> 8;
	val ^= val >> 4;
	return (0x9669u >> (val & 0xf)) & 1u;
}

static void
ring_put(uint32_t v)
{
	((volatile uint32_t *)ring_va)[ring_wptr] = v;
	ring_wptr = (ring_wptr + 1) % (RING_BYTES / 4);
}

static void
ring_pkt7(uint32_t opcode, uint32_t cnt)
{
	ring_put(0x70000000u | (cnt & 0x3fff) | (pm4_parity(cnt) << 15) |
	    ((opcode & 0x7f) << 16) | (pm4_parity(opcode) << 23));
}

/* Publish the packets and wait for the CP read pointer to catch up. */
static int
ring_submit(const char *what)
{
	uint32_t rptr = 0;

	flush_dcache64((addr64_t)ring_pa, RING_BYTES, TRUE);
	wr(gpu, A5_CP_RB_WPTR, ring_wptr);
	for (int i = 0; i < 300; i++) {
		rptr = rd(gpu, A5_CP_RB_RPTR);
		if (rptr == ring_wptr) {
			printf("LAVGPU: %s consumed: RPTR %u WPTR %u after %d ms\n",
			    what, rptr, ring_wptr, i * 10);
			return 0;
		}
		lav_wait(10);
	}
	printf("LAVGPU: %s NOT consumed: RPTR %u WPTR %u, RBBM_STATUS %08x INT_0 "
	    "%08x AHB_ERR %08x\n", what, rptr, ring_wptr, rd(gpu, A5_RBBM_STATUS),
	    rd(gpu, A5_RBBM_INT_0_STATUS), rd(gpu, A5_RBBM_AHB_ERR_STATUS));
	return -1;
}

static void
step11_cp_start(void)
{
	volatile uint32_t *fence;

	if (ring_va == 0 || ring_pa == 0) {
		printf("LAVGPU: step 11 needs the ring of step 10\n");
		return;
	}
	lav_announce("step 11: WRITE the CP/RBBM setup registers");
	wr(gpu, A5_SECVID_TSB_CNTL, 0);
	wr(gpu, A5_SECVID_TSB_BASE_LO, 0);
	wr(gpu, A5_SECVID_TSB_BASE_LO + 4, 0);
	wr(gpu, A5_SECVID_TSB_SIZE, 0);
	wr(gpu, A5_CP_ADDR_MODE_CNTL, 1);
	wr(gpu, A5_SECVID_TSB_ADDR_MODE, 1);
	wr(gpu, A5_CP_MEQ_THRESHOLDS, 0x40);
	wr(gpu, A5_CP_MERCIU_SIZE, 0x400);
	wr(gpu, A5_CP_ROQ_THRESH_2, 0x80000060);
	wr(gpu, A5_CP_ROQ_THRESH_1, 0x40201b16);
	wr(gpu, A5_CP_CHICKEN_DBG, 0x02000000);
	wr(gpu, A5_RBBM_INT_0_MASK, 0);
	wr(gpu, A5_CP_RB_CNTL, 12u | (2u << 8) | RB_NO_UPDATE);
	printf("LAVGPU: RB_CNTL %08x CP_ADDR_MODE %08x\n", rd(gpu, A5_CP_RB_CNTL),
	    rd(gpu, A5_CP_ADDR_MODE_CNTL));

	lav_announce("step 11: clear the PFP/ME halt");
	wr(gpu, A5_CP_PFP_ME_CNTL, 0);
	lav_wait(50);
	printf("LAVGPU: CP_PFP_ME_CNTL %08x RBBM_STATUS %08x RPTR %u WPTR %u\n",
	    rd(gpu, A5_CP_PFP_ME_CNTL), rd(gpu, A5_RBBM_STATUS),
	    rd(gpu, A5_CP_RB_RPTR), rd(gpu, A5_CP_RB_WPTR));

	lav_announce("step 11: submit CP_ME_INIT");
	ring_pkt7(CP_ME_INIT, 8);
	ring_put(0x0000002f);	/* max context */
	ring_put(0x00000003);	/* multiple hardware contexts */
	ring_put(0x20000000);	/* error detection */
	ring_put(0);
	ring_put(0);
	ring_put(0);		/* no microcode workarounds (a512) */
	ring_put(0);
	ring_put(0);
	if (ring_submit("ME_INIT") != 0) {
		return;
	}

	lav_announce("step 11: submit CP_SET_SECURE_MODE(0)");
	ring_pkt7(CP_SET_SECURE_MODE, 1);
	ring_put(0);
	if (ring_submit("SET_SECURE_MODE") != 0) {
		return;
	}
	printf("LAVGPU: SECVID_TRUST_CNTL now %08x RBBM_STATUS %08x\n",
	    rd(gpu, A5_RBBM_SECVID_TRUST), rd(gpu, A5_RBBM_STATUS));

	lav_announce("step 11: submit CP_MEM_WRITE (fence word)");
	fence = (volatile uint32_t *)(rptr_va + 0x100);
	*fence = 0;
	flush_dcache64((addr64_t)fence_pa, 64, TRUE);
	ring_pkt7(CP_MEM_WRITE, 3);
	ring_put((uint32_t)fence_pa);
	ring_put((uint32_t)(fence_pa >> 32));
	ring_put(0xcafe0001u);
	if (ring_submit("MEM_WRITE") != 0) {
		return;
	}
	flush_dcache64((addr64_t)fence_pa, 64, TRUE);
	printf("LAVGPU: fence word %08x (%s)\n", *fence,
	    *fence == 0xcafe0001u ? "CP WROTE IT" : "not written");
}

static void
lavender_gpu_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t step = 0, delay_s = 25;

	(void)PE_parse_boot_argn("lavgpu", &step, sizeof(step));
	(void)PE_parse_boot_argn("lavgpudelay", &delay_s, sizeof(delay_s));
	if (delay_s > 300) {
		delay_s = 300;
	}
	printf("LAVGPU: ladder up to step %u in %u s\n", step, delay_s);
	lav_wait(delay_s * 1000);

	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	gpucc = ml_io_map(GPUCC_PA, GPUCC_SIZE);
	if (gcc == 0 || gpucc == 0) {
		printf("LAVGPU: map failed gcc %p gpucc %p\n", (void *)gcc,
		    (void *)gpucc);
		return;
	}
	step1_reads();
	if (step >= 2) {
		step2_votes();
	}
	if (step >= 3 && step3_cx() != 0) {
		printf("LAVGPU: stopped before step 4 (CX or AHB not ready)\n");
		return;
	}
	if (step >= 4) {
		gpu = ml_io_map(GPU_PA, GPU_SIZE);
		if (gpu == 0) {
			printf("LAVGPU: GPU block map failed\n");
			return;
		}
		step4_gx();
	}
	if (step >= 5) {
		step5_scm();
	}
	if (step >= 6) {
		step6_zap();
	}
	if (step >= 7) {
		step7_smmu();
	}
	if (step >= 8) {
		step8_smmu_bypass();
	}
	if (step >= 9) {
		step9_smmu_s1();
	}
	if (step >= 10) {
		step10_ucode_ring();
	}
	if (step >= 11) {
		step11_cp_start();
	}
	printf("LAVGPU: ladder finished\n");
}

int
lavender_gpu_probe(void)
{
	static int started;
	uint32_t step = 0;
	thread_t thread;

	if (started || !PE_parse_boot_argn("lavgpu", &step, sizeof(step)) ||
	    step == 0) {
		return 0;
	}
	started = 1;
	if (kernel_thread_start(lavender_gpu_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_LAVENDER */

int lavender_gpu_probe(void);
int
lavender_gpu_probe(void)
{
	return 0;
}

#endif
