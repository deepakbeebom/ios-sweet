/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));
static int ufs_ours;	/* our transfer request list is live */

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void gpt_dump(void);

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}
	ufs_ours = 1;

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	gpt_dump();
}

/* LUN 0: protective MBR, GPT header and the larger partitions. Read-only. */
void gpt_dump(void)
{
	uint32_t ocs = 0, st = 0, rc;
	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}

#ifdef G0Q
/*
 * g0q milestone 4: why does every command end in CHECK CONDITION (boot 4)?
 * Lists the logical units, decodes sense data per unit and, if units are
 * NOT READY, puts the device into the Active power mode the way Linux's
 * ufshcd does (START STOP UNIT, POWER CONDITION 1, to the UFS Device W-LUN).
 * Still no SCSI WRITE anywhere.
 */
#define WLUN_REPORT_LUNS	0x81	/* UPIU LUN field for W-LUN 0xC101 */
#define WLUN_UFS_DEVICE		0xd0	/* UPIU LUN field for W-LUN 0xC150 */

/* key<<16 | asc<<8 | ascq from the last response, or 0xffffff if none. */
static uint32_t last_sense(void)
{
	uint32_t slen = (uint32_t)ucd.resp[32] << 8 | ucd.resp[33];
	if (slen < 14)
		return 0xffffff;
	const uint8_t *sd = ucd.resp + 34;
	return (uint32_t)(sd[2] & 0xf) << 16 | (uint32_t)sd[12] << 8 | sd[13];
}

static void result(const char *label, uint32_t rc, uint32_t ocs, uint32_t st)
{
	if (rc == 4 && st == 2)
		con_hex2(label, "CHECK key/asc/q=", last_sense(), 0, 0);
	else
		con_hex2(label, "rc=", rc, "ocs/st=", ocs << 8 | st);
}

static uint32_t tur(uint8_t lun, const char *label)
{
	static const uint8_t cdb[6] = { 0x00 };
	uint32_t ocs = 0, st = 0, rc = 0, sense = 0;
	/* Unit attentions are reported once each: retry a few. */
	for (int i = 0; i < 4; i++) {
		rc = scsi(lun, cdb, sizeof(cdb), 0, 0, &ocs, &st);
		sense = rc == 4 && st == 2 ? last_sense() : 0;
		if (!rc || (sense >> 16) != 6)
			break;
	}
	result(label, rc, ocs, st);
	return rc ? (sense ? sense : 0xff000000u | rc) : 0;
}

static void capacity(uint8_t lun)
{
	static const uint8_t cdb[10] = { 0x25 };
	uint32_t ocs = 0, st = 0;
	uint32_t rc = scsi(lun, cdb, sizeof(cdb), data, 8, &ocs, &st);
	if (rc)
		result("  cap", rc, ocs, st);
	else
		con_hex2("  cap", "lastlba=", be32(data), "blk=", be32(data + 4));
}

static void inquiry(uint8_t lun, char *label)
{
	static const uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
	uint32_t ocs = 0, st = 0;
	uint32_t rc = scsi(lun, cdb, sizeof(cdb), data, 36, &ocs, &st);
	if (rc) {
		result(label, rc, ocs, st);
		return;
	}
	char prod[25];
	for (int i = 0; i < 24; i++) {	/* vendor (8) + product (16) */
		uint8_t c = data[8 + i];
		prod[i] = c >= 32 && c < 127 ? c : '.';
	}
	prod[24] = 0;
	con_str(label, prod);
}

void test_ufs_luns(void)
{
	if (!ufs_ours) {
		con_str("UFS LUNS", "no request list: skip");
		return;
	}
	con_skip();

	/* REPORT LUNS (select report 0: logical units) */
	static const uint8_t rl[12] = { 0xa0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0 };
	uint32_t ocs = 0, st = 0;
	uint32_t rc = 0;
	for (int i = 0; i < 3; i++) {
		rc = scsi(WLUN_REPORT_LUNS, rl, sizeof(rl), data, 64, &ocs, &st);
		if (!rc || !(rc == 4 && st == 2 && (last_sense() >> 16) == 6))
			break;
	}
	uint32_t mask = 0;
	if (rc) {
		result("REPORTLUN", rc, ocs, st);
		mask = 0xff;	/* fall back to LUNs 0..7 */
	} else {
		uint32_t n = be32(data) / 8;
		for (uint32_t i = 0; i < n && i < 7; i++)
			if (data[8 + i * 8 + 1] < 8)
				mask |= 1u << data[8 + i * 8 + 1];
		con_hex2("REPORTLUN", "count=", n, "mask=", mask);
	}

	int not_ready = 0;
	for (uint8_t lun = 0; lun < 8; lun++) {
		if (!(mask & (1u << lun)))
			continue;
		char label[] = "LUN0";
		label[3] = '0' + lun;
		inquiry(lun, label);
		uint32_t t = tur(lun, "  tur");
		if ((t >> 16) == 2)
			not_ready = 1;
		if (!t)
			capacity(lun);
	}

	if (!not_ready)
		return;
	con_skip();
	/* START STOP UNIT, POWER CONDITION = 1 (Active), as ufshcd_set_dev_pwr_mode */
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	for (int i = 0; i < 3; i++) {
		rc = scsi(WLUN_UFS_DEVICE, ssu, sizeof(ssu), 0, 0, &ocs, &st);
		if (!rc || !(rc == 4 && st == 2 && (last_sense() >> 16) == 6))
			break;
	}
	result("SSU ACTIVE", rc, ocs, st);
	for (uint8_t lun = 0; lun < 8; lun++) {
		if (!(mask & (1u << lun)))
			continue;
		char label[] = "LUN0 tur";
		label[3] = '0' + lun;
		if (!tur(lun, label))
			capacity(lun);
	}
}
#endif

#ifdef G0Q
/* Wake the UFS device (boot 5: all LUNs NOT READY until this). Returns 0 on success. */
uint32_t ufs_wake(void)
{
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	uint32_t ocs = 0, st = 0, rc = 1;
	if (!ufs_ours) {
		con_str("UFS WAKE", "no request list: skip");
		return rc;
	}
	for (int i = 0; i < 3; i++) {
		rc = scsi(WLUN_UFS_DEVICE, ssu, sizeof(ssu), 0, 0, &ocs, &st);
		if (!rc || !(rc == 4 && st == 2 && (last_sense() >> 16) == 6))
			break;
	}
	result("SSU ACTIVE", rc, ocs, st);
	if (!rc && tur(0, "LUN0 tur"))
		rc = 5;
	return rc;
}
#endif

#ifdef G0Q
/* Read LUN 0 block 1 (GPT header) by DMA straight to physical address pa. */
uint32_t ufs_dma_test(uint64_t pa)
{
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
	volatile uint8_t *dst = (volatile uint8_t *)pa;
	uint32_t ocs = 0, st = 0;
	for (int i = 0; i < 16; i++)
		dst[i] = 0x5a;
	uint32_t rc = scsi(0, rd10, sizeof(rd10), (void *)pa, BLOCK, &ocs, &st);
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = dst[i] >= 32 && dst[i] < 127 ? dst[i] : '.';
	sig[8] = 0;
	con_hex2("DMA HIGH", "pa_hi=", (uint32_t)(pa >> 32), "rc=", rc);
	con_str("DMA HIGH", sig);
	return rc || dst[0] != 'E' || dst[1] != 'F' || dst[2] != 'I';
}
#endif
