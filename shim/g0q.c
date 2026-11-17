/*
 * Galaxy S22+ (g0q, SM8450) milestone 1: our code runs and the screen shows it.
 *
 * The panel (S6E3FAB) is a DSI command-mode panel, so pixels written to the
 * buffer appear only when the display controller sends a frame. As on the
 * Pixel 4 port (ios-flame sde.c), we find the pipe the bootloader left
 * scanning out, draw into ITS buffer, and switch on INTF_1's tear-check
 * autorefresh so every TE pulse sends a frame. If tear check is off, we kick
 * CTL_START by hand after each redraw instead.
 *
 * Writes: the framebuffer, INTF_1 autorefresh (or CTL_START). Nothing else:
 * no storage access, no PMIC, no reset. The shim idles forever at the end;
 * leave with a forced restart (Volume down + Power) into Download mode.
 *
 * Offsets: qcom,sde-kms in the stock S906E dtbo (board-id 12), mdp_phys
 * 0xae00000: ctl 0x16000 + 0x1000 n, sspp vig 0x5000/7000/9000/b000,
 * dma 0x25000/27000/29000/2b000, intf 0x35000 + 0x1000 n (intf1 = DSI0).
 */
#include <stdint.h>
#include "console.h"

#define MDP		0x0ae00000UL
#define CTL(n)		(MDP + 0x16000 + 0x1000 * (n))
#define INTF(n)		(MDP + 0x35000 + 0x1000 * (n))
#define NR_CTL		6

#define CTL_FLUSH	0x018
#define CTL_START	0x01c
#define CTL_INTF_ACTIVE	0x0f4

#define SSPP_SRC_SIZE	0x00
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_FORMAT	0x30

#define INTF_TEAR_EN		0x284
#define INTF_TEAR_INT_COUNT	0x298
#define INTF_TEAR_LINE_COUNT	0x2b0
#define INTF_TEAR_AUTOREFRESH	0x2b4

#define SPLASH_BASE	0xb8000000UL	/* reserved-memory cont_splash_region */
#define SPLASH_SIZE	0x02b00000UL

#define PSCI_VERSION	0x84000000UL
#define SMCCC_VERSION	0x80000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t smc0(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	return x0;
}

static uint64_t ticks(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntvct_el0");
}

static uintptr_t sspp_base(int i)
{
	static const uint32_t off[8] = { 0x5000, 0x7000, 0x9000, 0xb000,
					 0x25000, 0x27000, 0x29000, 0x2b000 };
	return MDP + off[i];
}

struct disp {
	uintptr_t ctl, pipe, fb;
	uint32_t ctl_idx, intf_active, pipe_idx, size, stride, format, tear_en;
	int manual;	/* 1: kick CTL_START after each redraw */
};

static void disp_probe(struct disp *d)
{
	for (int i = 0; i < NR_CTL && !d->ctl; i++) {
		uint32_t intf = rd32(CTL(i) + CTL_INTF_ACTIVE);
		if (intf & (1u << 1)) {	/* INTF_1 = DSI0 */
			d->ctl = CTL(i);
			d->ctl_idx = i;
			d->intf_active = intf;
		}
	}
	for (int i = 0; i < 8 && !d->pipe; i++) {
		uintptr_t b = sspp_base(i);
		uint32_t size = rd32(b + SSPP_SRC_SIZE);
		uint32_t addr = rd32(b + SSPP_SRC0_ADDR);
		if (!size || !addr)
			continue;
		d->pipe = b;
		d->pipe_idx = i;
		d->size = size;
		d->fb = addr;
		d->stride = rd32(b + SSPP_YSTRIDE0) & 0xffff;
		d->format = rd32(b + SSPP_FORMAT);
	}
	d->tear_en = rd32(INTF(1) + INTF_TEAR_EN);
}

/* Only draw into the bootloader's own buffer, 32 bpp, inside the splash carve-out. */
static int disp_usable(const struct disp *d)
{
	return d->fb >= SPLASH_BASE && d->stride >= FB_W * 4 &&
	       d->fb + (uint64_t)d->stride * FB_H <= SPLASH_BASE + SPLASH_SIZE;
}

static void disp_show(const struct disp *d)
{
	if (d->manual && d->ctl)
		wr32(d->ctl + CTL_START, 1);
}

static struct disp gd;

/* Find the running display path, point the console at it, start autorefresh. */
void g0q_display(void)
{
	disp_probe(&gd);
	if (disp_usable(&gd))
		con_set(gd.fb, gd.stride);
	else
		con_set(SPLASH_BASE, FB_W * 4);
	if (gd.tear_en)
		wr32(INTF(1) + INTF_TEAR_AUTOREFRESH, (1u << 31) | 1u);
	else
		gd.manual = 1;
}

void g0q_show(void)
{
	disp_show(&gd);
}

void g0q_cpu_lines(uint64_t dtb)
{
	con_hex2("EL/MIDR", "el=", (uint32_t)(rsr("CurrentEL") >> 2), "midr=", (uint32_t)rsr("midr_el1"));
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex2("DTB", "at=", (uint32_t)dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
}

/* ---- live FDT: /memory banks written by the bootloader ---- */

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int name_is_memory(uintptr_t p)
{
	static const char m[] = "memory";
	for (int i = 0; i < 6; i++)
		if (*(volatile char *)(p + i) != m[i])
			return 0;
	char c = *(volatile char *)(p + 6);
	return c == 0 || c == '@';
}

static uintptr_t skip_str(uintptr_t p)
{
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static int str_is(uintptr_t p, const char *s)
{
	while (*s && *(volatile char *)p == *s)
		p++, s++;
	return *s == 0 && *(volatile char *)p == 0;
}

/*
 * Prints every (base, size) pair of every top-level memory node, in MiB,
 * plus the total. Root #address-cells and #size-cells are 2 on SM8450.
 */
void g0q_memory(uint64_t dtb)
{
	if (!dtb || be32(dtb) != 0xd00dfeed) {
		con_str("MEM", "no valid DTB");
		return;
	}
	uintptr_t p = dtb + be32(dtb + 8);
	uintptr_t strings = dtb + be32(dtb + 12);
	int depth = 0, in_mem = 0, banks = 0, shown = 0;
	uint64_t total = 0;

	for (int guard = 0; guard < 2000000; guard++) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {			/* BEGIN_NODE */
			depth++;
			in_mem = depth == 2 && name_is_memory(p);
			p = (skip_str(p) + 3) & ~3UL;
		} else if (tok == 2) {		/* END_NODE */
			if (depth == 2)
				in_mem = 0;
			depth--;
		} else if (tok == 3) {		/* PROP */
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_mem && str_is(strings + nameoff, "reg")) {
				for (uint32_t o = 0; o + 16 <= len; o += 16) {
					uint64_t base = (uint64_t)be32(val + o) << 32 | be32(val + o + 4);
					uint64_t size = (uint64_t)be32(val + o + 8) << 32 | be32(val + o + 12);
					if (!size)
						continue;
					banks++;
					total += size;
					if (shown < 12) {
						con_hex2("MEM MiB", "at=", (uint32_t)(base >> 20), "len=", (uint32_t)(size >> 20));
						shown++;
					}
				}
			}
			p = (val + len + 3) & ~3UL;
		} else if (tok == 4) {		/* NOP */
			continue;
		} else {			/* END or garbage */
			break;
		}
	}
	con_hex2("MEM", "banks=", banks, "totalMiB=", (uint32_t)(total >> 20));
}

/* Heartbeat from row `row` on: proves we are alive and frames still reach the panel. */
void __attribute__((noreturn)) g0q_idle(int row)
{
	uint64_t freq = rsr("cntfrq_el0");
	if (!freq)
		freq = 19200000;
	g0q_show();
	for (uint32_t n = 0;; n++) {
		con_row(row);
		con_hex2("ALIVE", "sec=", n, "te=", rd32(INTF(1) + INTF_TEAR_INT_COUNT));
		g0q_show();
		uint64_t end = ticks() + freq;
		while (ticks() < end)
			;
	}
}

/* Milestone 1: display path and CPU facts (boot 1, 2026-11-17). */
void __attribute__((noreturn)) g0q_main(uint64_t dtb)
{
	g0q_display();
	struct disp d = gd;

	con_clear(0xff800000U);	/* dark red: our code is running */
	con_str("SHIM", "g0q milestone 1: S22+ display");
	con_skip();
	g0q_cpu_lines(dtb);
	con_hex2("MMFR0", "tg16=", (uint32_t)(rsr("id_aa64mmfr0_el1") >> 20) & 0xf,
		 "pa=", (uint32_t)rsr("id_aa64mmfr0_el1") & 0xf);
	con_hex2("ISAR1", "apa=", (uint32_t)(rsr("id_aa64isar1_el1") >> 4) & 0xf,
		 "api=", (uint32_t)(rsr("id_aa64isar1_el1") >> 8) & 0xf);
	con_hex2("DTB hi", "at=", (uint32_t)(dtb >> 32), "cntfrq=", (uint32_t)rsr("cntfrq_el0"));
	con_skip();
	con_hex2("MDP", "hw=", rd32(MDP), "ctl=", d.ctl_idx);
	con_hex2("CTL", "intf=", d.intf_active, "flush=", d.ctl ? rd32(d.ctl + CTL_FLUSH) : 0);
	con_hex2("PIPE", "idx=", d.pipe_idx, "size=", d.size);
	con_hex2("FB", "addr=", (uint32_t)d.fb, "stride=", d.stride);
	con_hex2("FMT", "fmt=", d.format, "ok=", disp_usable(&d));
	con_hex2("TEAR", "en=", d.tear_en, "auto=", rd32(INTF(1) + INTF_TEAR_AUTOREFRESH));
	con_skip();
	con_hex2("PSCI", "ver=", (uint32_t)smc0(PSCI_VERSION), "smccc=", (uint32_t)smc0(SMCCC_VERSION));
	con_skip();
	con_str("NEXT", "hold VolDown+Power to restart,");
	con_str("", "then VolUp+VolDown + USB: Download");
	g0q_idle(20);
}
