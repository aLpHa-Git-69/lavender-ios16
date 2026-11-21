/*
 * lavender: Wi-Fi (WCN3990) platform probe, F0b/F2. Boot-arg gated.
 *
 *   sweetwlan=1       run the F0b platform probe (read only)
 *   sweetwlan=2       run the F2 SMEM/GLINK probe after the modem auths
 *   sweetwlandelay=S  seconds to wait after boot before the F0b probe
 *                     (default 25), so the console ring flusher and the USB
 *                     live log are up
 *
 * The WCN3990 is an icnss child of the ANOC2 SMMU (SIDs 0x1a00/0x1a01), not
 * of the apps SMMU: in the DTB the icnss iommus phandle resolves to
 * /soc/arm,smmu-anoc2@16c0000. The modem DSP runs wlanmdsp.mbn and talks to
 * the chip over the CE/HTC transport. Before writing any of that, this probe
 * answers the questions that decide the port:
 *
 *   - did the bootloader leave the MSS GCC branches on (0x8a000 cfg_ahb,
 *     0x8a004 mnoc_bimc_axi, 0x8a03c snoc_axi, 0x8a040 q6_bimc_axi)?
 *   - what does the ANOC2 SMMU have for stream IDs 0x1a00/0x1a01, and is it
 *     translating, in bypass or faulting?
 *   - is the SMEM header at 0x86000000 intact?
 *   - is the WLAN MSA carve-out at 0x85700000 still zero, or did the
 *     bootloader leave something there?
 *
 * F2 (`sweetwlan=2`) is called from the modem thread after AUTH_COMPLETE and
 * reads the SMEM v12 $TOC/$PRT, the APPS/MODEM partition items, SMP2P
 * 428/435, the GLINK descriptor 478/FIFOs 479-480 and the APCS IPC register.
 * It also READS ONLY: it does not allocate SMEM items, take the hwlock or
 * ring a doorbell. Those come after this probe says what the modem created.
 *
 * The QDSP6 (0x04080000) and RMB (0x04180000) blocks are not touched by the
 * F0b probe: the modem is still in reset, and a read there can hang the bus
 * or reset the SoC. Every step is announced before the access and spaced 2 s
 * so the log leaves the machine even if the access kills it.
 *
 * Register meanings: Linux gcc-sdm660.c, arm-smmu.c and glink_smem_native
 * xprt.c, read as a behaviour reference (values only, no code copied); DTB
 * values from
 * work/briefs/wifi-wcn3990-brief.md section 1.
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

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);

#define GCC_PA          0x00100000u
#define GCC_SIZE        0x94000u
#define ANOC2_PA        0x016c0000u
#define ANOC2_SIZE      0x40000u
#define SMEM_PA         0x86000000u
#define SMEM_SIZE       0x00001000u
#define MSA_PA          0x85700000u
#define MSA_SIZE        0x00001000u

/* F2: the whole SMEM v12 window, the APCS IPC register and the item IDs */
#define SMEM_F2_SIZE     0x00200000u
#define SMEM_TOC_OFF     (SMEM_F2_SIZE - 0x1000u)
#define APCS_PA          0x17911000u
#define APCS_SIZE        0x00001000u
#define APCS_IPC_OFF     0x8u
#define SMEM_HOST_APPS   0x0000u
#define SMEM_HOST_MODEM  0x0001u
#define ITEM_SMP2P_OUT   428u
#define ITEM_SMP2P_IN    435u
#define ITEM_GLINK_DESC  478u
#define ITEM_GLINK_TX    479u
#define ITEM_GLINK_RX    480u
#define SMP2P_MAGIC      0x504d5324u     /* "$SMP" */

/* SMEM v12 (qcom_smem): TOC at the end of the window, partitions in it. */
struct smem_pt_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, rsvd[7];
} __attribute__((packed));
struct smem_pt {
	uint8_t magic[4];
	uint32_t version, num_entries, rsvd[5];
} __attribute__((packed));
struct smem_part_hdr {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, rsvd[3];
} __attribute__((packed));
struct smem_priv_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t pad_data, pad_hdr;
	uint32_t rsvd;
} __attribute__((packed));
struct smp2p_hdr {
	uint32_t magic;
	uint8_t version, features[3];
	uint16_t local_pid, remote_pid, total, valid;
	uint32_t flags;
} __attribute__((packed));
struct smp2p_entry {
	char name[16];
	uint32_t value;
} __attribute__((packed));

/* GCC byte offsets from GCC_PA (gcc-sdm660.c) */
#define GCC_MSS_CFG_AHB         0x8a000u
#define GCC_MSS_MNOC_BIMC_AXI   0x8a004u
#define GCC_MSS_SNOC_AXI        0x8a03cu
#define GCC_MSS_Q6_BIMC_AXI     0x8a040u
#define CBCR_EN                 (1u << 0)
#define CBCR_OFF                (1u << 31)

/* ANOC2 SMMU GR0 (arm-smmu.c, qcom,smmu-v2) */
#define SMMU_SCR0               0x0000u
#define SMMU_SACR               0x0010u
#define SMMU_IDR0               0x0020u
#define SMMU_IDR1               0x0024u
#define SMMU_IDR7               0x003cu
#define SMMU_SGFAR              0x0040u
#define SMMU_SGFSR              0x0048u
#define SMMU_SMR(n)             (0x0800u + ((n) << 2))
#define SMMU_S2CR(n)            (0x0c00u + ((n) << 2))
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_SMR_MASK_SHIFT     16
#define SMMU_SMR_MASK           0x7fffu
#define SMMU_SMR_ID             0x7fffu
#define SMMU_S2CR_TYPE_SHIFT    16
#define SMMU_S2CR_CBNDX         0xffu
#define SMMU_MAX_SMRS           128
#define WLAN_SID0               0x1a00u
#define WLAN_SID1               0x1a01u

static vm_offset_t gcc, smmu, smem, msa, apcs;
static int wlan_chan;
static uint32_t wlan_mode;

void lavender_wlan_after_modem(void);

static inline uint32_t
wlan_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static void
wlan_wait(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&wlan_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
wlan_announce(const char *what)
{
	printf("SWEET WLAN: about to %s\n", what);
	wlan_wait(2000);
}

static void
wlan_gcc(void)
{
	static const struct {
		const char *name;
		uint32_t off;
	} branches[] = {
		{ "mss_cfg_ahb",      GCC_MSS_CFG_AHB },
		{ "mss_mnoc_bimc_axi", GCC_MSS_MNOC_BIMC_AXI },
		{ "mss_snoc_axi",     GCC_MSS_SNOC_AXI },
		{ "mss_q6_bimc_axi",  GCC_MSS_Q6_BIMC_AXI },
	};
	unsigned on = 0;

	wlan_announce("read the GCC MSS branches (read only)");
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		uint32_t v = wlan_rd(gcc, branches[i].off);

		if (!(v & CBCR_OFF)) {
			on++;
		}
		printf("SWEET WLAN: gcc %s %08x (en %u, %s)\n", branches[i].name,
		    v, v & CBCR_EN, (v & CBCR_OFF) ? "OFF" : "on");
	}
	printf("SWEET WLAN: %u of 4 MSS branches are on\n", on);
}

static const char *
wlan_smmu_type(uint32_t s2cr)
{
	switch ((s2cr >> SMMU_S2CR_TYPE_SHIFT) & 3u) {
	case 0:
		return "translate";
	case 1:
		return "bypass";
	case 2:
		return "fault";
	default:
		return "reserved";
	}
}

static void
wlan_smmu(void)
{
	uint32_t idr0, idr1, n, valid = 0, wlan = 0;

	wlan_announce("read the ANOC2 SMMU global registers");
	idr0 = wlan_rd(smmu, SMMU_IDR0);
	idr1 = wlan_rd(smmu, SMMU_IDR1);
	printf("SWEET WLAN: ANOC2 sCR0 %08x sACR %08x IDR0 %08x IDR1 %08x "
	    "IDR7 %08x\n", wlan_rd(smmu, SMMU_SCR0), wlan_rd(smmu, SMMU_SACR),
	    idr0, idr1, wlan_rd(smmu, SMMU_IDR7));
	printf("SWEET WLAN: ANOC2 sGFAR %08x sGFSR %08x\n",
	    wlan_rd(smmu, SMMU_SGFAR), wlan_rd(smmu, SMMU_SGFSR));

	/*
	 * IDR0[7:0] is NUMSMRG. The architecture maximum is 128; a zero or a
	 * bogus count falls back to the full table so a WLAN SMR cannot hide.
	 */
	n = idr0 & 0xffu;
	if (n == 0 || n > SMMU_MAX_SMRS) {
		n = SMMU_MAX_SMRS;
	}
	wlan_announce("scan the SMMU stream-mapping table (SMR/S2CR)");
	for (uint32_t i = 0; i < n; i++) {
		uint32_t smr = wlan_rd(smmu, SMMU_SMR(i));
		uint32_t s2cr, id, mask;

		if (!(smr & SMMU_SMR_VALID)) {
			continue;
		}
		valid++;
		id = smr & SMMU_SMR_ID;
		mask = (smr >> SMMU_SMR_MASK_SHIFT) & SMMU_SMR_MASK;
		s2cr = wlan_rd(smmu, SMMU_S2CR(i));
		printf("SWEET WLAN: SMR[%u] %08x sid %04x mask %04x -> "
		    "S2CR %08x %s cb %u\n", i, smr, id, mask, s2cr,
		    wlan_smmu_type(s2cr), s2cr & SMMU_S2CR_CBNDX);
		if (((WLAN_SID0 & ~mask) == (id & ~mask)) ||
		    ((WLAN_SID1 & ~mask) == (id & ~mask))) {
			wlan++;
			printf("SWEET WLAN: SMR[%u] covers WLAN sid %04x\n", i,
			    id);
		}
	}
	printf("SWEET WLAN: SMMU %u SMRs scanned, %u valid, %u cover the "
	    "WLAN SIDs\n", n, valid, wlan);
}

/* Count nonzero words over the whole mapping, then dump the first 0x200. */
static void
wlan_region(const char *name, vm_offset_t base, uint32_t size)
{
	uint32_t off, nonzero = 0;

	for (off = 0; off < size; off += 4) {
		if (wlan_rd(base, off) != 0) {
			nonzero++;
		}
	}
	printf("SWEET WLAN: %s: %u of %u words are nonzero\n", name, nonzero,
	    size / 4);
	for (off = 0; off < 0x200 && off < size; off += 32) {
		printf("SWEET WLAN: %s %03x: %08x %08x %08x %08x %08x %08x "
		    "%08x %08x\n", name, off, wlan_rd(base, off),
		    wlan_rd(base, off + 4), wlan_rd(base, off + 8),
		    wlan_rd(base, off + 12), wlan_rd(base, off + 16),
		    wlan_rd(base, off + 20), wlan_rd(base, off + 24),
		    wlan_rd(base, off + 28));
	}
}

/* --- F2: SMEM / GLINK / SMP2P read-only probe ----------------------------- */

static void
wlan_magic4(const uint8_t *m, char out[5])
{
	for (int i = 0; i < 4; i++) {
		out[i] = (m[i] >= 0x20 && m[i] < 0x7f) ? (char)m[i] : '.';
	}
	out[4] = 0;
}

static struct smem_part_hdr *
wlan_f2_partition(uint8_t *base, uint16_t a, uint16_t b)
{
	struct smem_pt *pt = (struct smem_pt *)(base + SMEM_TOC_OFF);
	struct smem_pt_entry *e = (struct smem_pt_entry *)(pt + 1);
	uint32_t i, n;

	if (memcmp(pt->magic, "$TOC", 4) != 0) {
		return NULL;
	}
	n = pt->num_entries;
	if (n > 64) {
		n = 64;
	}
	for (i = 0; i < n; i++) {
		if (e[i].offset == 0 || e[i].size == 0 ||
		    e[i].offset >= SMEM_F2_SIZE) {
			continue;
		}
		if ((e[i].host0 == a && e[i].host1 == b) ||
		    (e[i].host0 == b && e[i].host1 == a)) {
			return (struct smem_part_hdr *)(base + e[i].offset);
		}
	}
	return NULL;
}

static void
wlan_f2_toc(uint8_t *base)
{
	struct smem_pt *pt = (struct smem_pt *)(base + SMEM_TOC_OFF);
	struct smem_pt_entry *e = (struct smem_pt_entry *)(pt + 1);
	char magic[5];
	uint32_t i, n;

	if (memcmp(pt->magic, "$TOC", 4) != 0) {
		wlan_magic4((const uint8_t *)pt->magic, magic);
		printf("SWEET WLAN: no $TOC at %08x (got %s)\n",
		    SMEM_PA + SMEM_TOC_OFF, magic);
		return;
	}
	n = pt->num_entries;
	printf("SWEET WLAN: $TOC ver %u entries %u\n", pt->version, n);
	if (n > 64) {
		n = 64;
	}
	for (i = 0; i < n; i++) {
		struct smem_part_hdr *p;
		char pm[5];

		if (e[i].offset == 0 || e[i].size == 0) {
			continue;
		}
		printf("SWEET WLAN: toc[%u] off %05x size %05x host %04x/%04x "
		    "flags %x\n", i, e[i].offset, e[i].size, e[i].host0,
		    e[i].host1, e[i].flags);
		/*
		 * 0713: only read partitions with an APPS host. The 0712 boot
		 * hung on the first modem<->adsp partition (0x1/0x2) header;
		 * cross-subsystem SMEM is not readable from the AP.
		 */
		if (e[i].host0 != SMEM_HOST_APPS &&
		    e[i].host1 != SMEM_HOST_APPS) {
			printf("SWEET WLAN:   (no APPS host, not read)\n");
			continue;
		}
		if (e[i].offset >= SMEM_F2_SIZE - sizeof(*p)) {
			continue;
		}
		p = (struct smem_part_hdr *)(base + e[i].offset);
		wlan_magic4((const uint8_t *)p->magic, pm);
		printf("SWEET WLAN:   part %s host %04x/%04x size %x free %x/%x\n",
		    pm, p->host0, p->host1, p->size, p->free_uncached,
		    p->free_cached);
	}
}

static void *
wlan_f2_item(struct smem_part_hdr *p, uint16_t item, uint32_t *size)
{
	uint8_t *e, *end;

	if (p == NULL || memcmp(p->magic, "$PRT", 4) != 0 ||
	    p->free_uncached > p->size) {
		return NULL;
	}
	e = (uint8_t *)p + sizeof(*p);
	end = (uint8_t *)p + p->free_uncached;
	while (e + sizeof(struct smem_priv_entry) <= end) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)e;

		if (h->canary != 0xa5a5) {
			return NULL;
		}
		if (h->item == item) {
			if (size != NULL) {
				*size = h->size - h->pad_data;
			}
			return e + sizeof(*h) + h->pad_hdr;
		}
		e += sizeof(*h) + h->pad_hdr + h->size;
	}
	return NULL;
}

static void
wlan_f2_items(struct smem_part_hdr *p)
{
	uint8_t *e, *end;
	unsigned n = 0;

	if (memcmp(p->magic, "$PRT", 4) != 0 || p->free_uncached > p->size) {
		char pm[5];

		wlan_magic4((const uint8_t *)p->magic, pm);
		printf("SWEET WLAN: partition is not $PRT (%s, free %x/%x)\n",
		    pm, p->free_uncached, p->size);
		return;
	}
	e = (uint8_t *)p + sizeof(*p);
	end = (uint8_t *)p + p->free_uncached;
	while (e + sizeof(struct smem_priv_entry) <= end && n < 96) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)e;

		if (h->canary != 0xa5a5) {
			printf("SWEET WLAN: item walk stops at %u (canary %04x)\n",
			    n, h->canary);
			return;
		}
		printf("SWEET WLAN: item %u size %u pad %u/%u\n", h->item,
		    h->size, h->pad_data, h->pad_hdr);
		if (h->item == ITEM_GLINK_DESC && h->size >= 32) {
			uint32_t *d = (uint32_t *)(void *)(e + sizeof(*h) +
			    h->pad_hdr);

			printf("SWEET WLAN: glink desc %08x %08x %08x %08x "
			    "%08x %08x %08x %08x\n", d[0], d[1], d[2], d[3],
			    d[4], d[5], d[6], d[7]);
		}
		e += sizeof(*h) + h->pad_hdr + h->size;
		n++;
	}
	printf("SWEET WLAN: %u items, free_uncached %x free_cached %x\n", n,
	    p->free_uncached, p->free_cached);
}

/*
 * 0713: the partition has an uncached area growing up from offset_free_uncached
 * and a cached area packed down from the end; qcom_smem walks the latter from
 * free_cached upward. One 0x4000-byte item is already there, so the GLINK
 * FIFO may live in this half.
 */
static void
wlan_f2_items_cached(struct smem_part_hdr *p)
{
	uint8_t *base = (uint8_t *)p;
	uint32_t pos = p->free_cached;
	unsigned n = 0;

	if (p->free_cached < sizeof(*p) || p->free_cached > p->size) {
		printf("SWEET WLAN: cached area %x/%x (not walked)\n",
		    p->free_cached, p->size);
		return;
	}
	while (pos + sizeof(struct smem_priv_entry) <= p->size && n < 64) {
		struct smem_priv_entry *h =
		    (struct smem_priv_entry *)(base + pos);

		if (h->canary != 0xa5a5) {
			printf("SWEET WLAN: cached walk stops at %u (canary "
			    "%04x)\n", n, h->canary);
			break;
		}
		printf("SWEET WLAN: cached item %u size %u pad %u/%u\n", h->item,
		    h->size, h->pad_data, h->pad_hdr);
		if (h->item == ITEM_GLINK_DESC && h->size >= 32) {
			uint32_t *d = (uint32_t *)(void *)(h + 1);

			printf("SWEET WLAN: glink desc(c) %08x %08x %08x %08x "
			    "%08x %08x %08x %08x\n", d[0], d[1], d[2], d[3],
			    d[4], d[5], d[6], d[7]);
		}
		pos += sizeof(*h) + h->pad_hdr + h->size;
		n++;
	}
	printf("SWEET WLAN: %u cached items from %x to %x\n", n,
	    p->free_cached, p->size);
}

static void
wlan_f2_smp2p(struct smem_part_hdr *p, uint16_t item, const char *label)
{
	uint32_t size = 0, total, avail, i;
	struct smp2p_hdr *h;
	struct smp2p_entry *ent;

	h = (struct smp2p_hdr *)wlan_f2_item(p, item, &size);
	if (h == NULL) {
		printf("SWEET WLAN: smp2p %s item %u absent\n", label, item);
		return;
	}
	if (size < sizeof(*h)) {
		printf("SWEET WLAN: smp2p %s item %u too small (%u)\n", label,
		    item, size);
		return;
	}
	printf("SWEET WLAN: smp2p %s item %u size %u magic %08x ver %u "
	    "pid %u/%u total %u valid %u flags %x\n", label, item, size,
	    h->magic, h->version, h->local_pid, h->remote_pid, h->total,
	    h->valid, h->flags);
	if (h->magic != SMP2P_MAGIC) {
		return;
	}
	total = h->total;
	avail = (size - sizeof(*h)) / sizeof(*ent);
	if (total > avail) {
		total = avail;
	}
	if (total > 16) {
		total = 16;
	}
	ent = (struct smp2p_entry *)(h + 1);
	for (i = 0; i < total; i++) {
		char name[17];

		memcpy(name, ent[i].name, 16);
		name[16] = 0;
		printf("SWEET WLAN: smp2p %s[%u] %s = %08x\n", label, i, name,
		    ent[i].value);
	}
}

/* Called by the modem thread after AUTH_COMPLETE, only for sweetwlan>=2. */
void
lavender_wlan_after_modem(void)
{
	struct smem_part_hdr *part = NULL;
	uint32_t i, size = 0;

	if (wlan_mode < 2) {
		return;
	}
	printf("SWEET WLAN: F2 SMEM/GLINK probe after modem auth\n");

	smem = ml_io_map_wcomb(SMEM_PA, SMEM_F2_SIZE);
	if (smem == 0) {
		printf("SWEET WLAN: SMEM 2 MiB map failed\n");
		return;
	}
	apcs = ml_io_map(APCS_PA, APCS_SIZE);
	if (apcs == 0) {
		printf("SWEET WLAN: APCS map failed\n");
	}

	wlan_f2_toc((uint8_t *)smem);

	/*
	 * The modem SMEM items (SMP2P 428/435, GLINK 478-480) live in the
	 * APPS<->MODEM partition, not in a GLOBAL one (0712 showed only host
	 * pairs 0/1, 0/2, 0/5, 1/2 and ffff/ffff in this TOC).
	 */
	for (i = 0; i < 20; i++) {
		part = wlan_f2_partition((uint8_t *)smem, SMEM_HOST_APPS,
		    SMEM_HOST_MODEM);
		if (part != NULL && wlan_f2_item(part, ITEM_SMP2P_IN, &size)) {
			break;
		}
		wlan_wait(500);
	}
	if (part == NULL) {
		printf("SWEET WLAN: no APPS/MODEM SMEM partition\n");
		return;
	}
	printf("SWEET WLAN: APPS/MODEM partition off %05x after %u ms\n",
	    (uint32_t)((uint8_t *)part - (uint8_t *)smem), i * 500);
	wlan_f2_items(part);
	wlan_f2_items_cached(part);
	wlan_f2_smp2p(part, ITEM_SMP2P_OUT, "out");
	wlan_f2_smp2p(part, ITEM_SMP2P_IN, "in");
	if (apcs != 0) {
		uint32_t v = wlan_rd(apcs, APCS_IPC_OFF);

		printf("SWEET WLAN: APCS IPC %08x (bit15 modem %u, bit9 adsp "
		    "%u)\n", v, (v >> 15) & 1, (v >> 9) & 1);
	}
	printf("SWEET WLAN: F2 probe done\n");
}

static void
lavender_wlan_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t delay_s = 25;

	(void)PE_parse_boot_argn("sweetwlandelay", &delay_s, sizeof(delay_s));
	if (delay_s > 300) {
		delay_s = 300;
	}
	if (wlan_mode >= 2) {
		printf("SWEET WLAN: F2 probe waits for the modem auth\n");
		return;
	}
	printf("SWEET WLAN: F0b platform probe in %u s\n", delay_s);
	wlan_wait(delay_s * 1000);

	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		printf("SWEET WLAN: GCC map failed\n");
		return;
	}
	wlan_gcc();

	smmu = ml_io_map(ANOC2_PA, ANOC2_SIZE);
	if (smmu == 0) {
		printf("SWEET WLAN: ANOC2 SMMU map failed\n");
	} else {
		wlan_smmu();
	}

	smem = ml_io_map(SMEM_PA, SMEM_SIZE);
	if (smem == 0) {
		printf("SWEET WLAN: SMEM map failed\n");
	} else {
		wlan_announce("dump the SMEM header");
		wlan_region("SMEM", smem, SMEM_SIZE);
	}

	msa = ml_io_map(MSA_PA, MSA_SIZE);
	if (msa == 0) {
		printf("SWEET WLAN: MSA map failed\n");
	} else {
		wlan_announce("dump the WLAN MSA carve-out");
		wlan_region("MSA", msa, MSA_SIZE);
	}
	printf("SWEET WLAN: F0b probe done\n");
}

int
lavender_wlan_probe(void)
{
	static int started;
	uint32_t on = 0;
	thread_t thread;

	if (started ||
	    !PE_parse_boot_argn("sweetwlan", &on, sizeof(on)) || on == 0) {
		return 0;
	}
	started = 1;
	wlan_mode = on;
	if (kernel_thread_start(lavender_wlan_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_LAVENDER */

int lavender_wlan_probe(void);
void lavender_wlan_after_modem(void);
int
lavender_wlan_probe(void)
{
	return 0;
}

void
lavender_wlan_after_modem(void)
{
}

#endif
