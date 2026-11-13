// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * An emulated ADSP-SC58x boot ROM OTP API (include/adi_rom.h), so that the
 * real ADSP-SC58x OTP code (adi-otp-sc58x.c) runs on sandbox for the tests
 * in test/dm/adi_otp.c.
 *
 * It is a model of the ROM API as adi-otp-sc58x.c uses it, not of the
 * ROM itself, on an emulated OTP array:
 *  - otp_pgm() programs the fields of otp_data half-word by half-word,
 *    skipping null addresses and zero values, and always the anti-rollback
 *    counter, failing if its count is below the current one;
 *  - a half-word can only be programmed once (or again with the same
 *    value);
 *  - otp_get() returns the counter's count (the only command the driver
 *    uses), otp_get_data() raw words, and otp_lock() programs the lock
 *    word, effective at the next (emulated) reset.
 * The OTPC, RCU and CGU registers are plain sandbox memory at the
 * addresses of asm/test.h and test.dts; faults are injected with
 * sandbox_adi_rom_inject().
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <adi_rom.h>
#include <dm.h>
#include <mapmem.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/test.h>

/* The OTP */
#define SB_OTP_SIZE		0x490	/* to the lock word */
#define SB_ANTIROLL		0x2c0
#define SB_ANTIROLL_MAX		32
#define SB_ANTIROLL_UNIT	1	/* a programmed half-word: one count */
#define SB_GP1			0x300
#define SB_GP1_WORDS		16
#define SB_BOOTMODEDISABLE	0x340
#define SB_PREBOOT_DDR_CFG	0x344
#define SB_PREBOOT_DDR_CFG_WORDS 12
#define SB_STAGEID		0x374
#define SB_STAGEID_WORDS	2
#define SB_FSN			0x380
#define SB_FSN_BYTES		16
#define SB_LOCK			0x48c
#define SB_LOCK_BIT		BIT(0)
#define SB_OTPCMD_ANTIROLL	15	/* otp_get(): the counter's count */

/* Registers, each block SB_REGS_SIZE bytes */
#define SB_REGS_SIZE		0x100
#define SB_OTPC_CTL		0x00
#define SB_OTPC_CTL_READMODE	BIT(0)
#define SB_OTPC_STAT		0x04
#define SB_OTPC_STAT_ADDRERR	BIT(13)
#define SB_OTPC_SECU_STATE	0x2c
#define SB_OTPC_SECU_LOCKED	1
#define SB_OTPC_PMC_STAT	0x84
#define SB_PMC_STAT_STATE	GENMASK(7, 6)
#define SB_PMC_IDLE		FIELD_PREP(SB_PMC_STAT_STATE, 2)
#define SB_PMC_BUSY		FIELD_PREP(SB_PMC_STAT_STATE, 1)
#define SB_RCU_STAT		0x04
#define SB_RCU_STAT_BMODE	GENMASK(11, 8)
#define SB_BMODE_SPI_MASTER	1
#define SB_RCU_CRCTL		0x08
#define SB_RCU_CRSTAT		0x0c
#define SB_RCU_SISTAT		0x14
#define SB_RCU_SI_SHARCS	(BIT(0) | BIT(1))	/* SIDIS/SISTAT bits */
#define SB_CGU_CTL		0x00
#define SB_CGU_CTL_MSEL		GENMASK(14, 8)
#define SB_CGU_DIV		0x0c
#define SB_CGU_DIV_CSEL		GENMASK(4, 0)
#define SB_CGU_DIV_SYSSEL	GENMASK(12, 8)
#define SB_CGU_DIV_DSEL		GENMASK(20, 16)

/* The address members of otp_data, in order, with their fields */
static const struct {
	u16 offset;
	u8 words;
} sb_fields[] = {
	{ 0x000, 8 },	/* huk */
	{ 0x020, 40 },	/* DTCP_key_ecc */
	{ 0x0c0, 10 },	/* DTCP_key_cont */
	{ 0x0e8, 32 },	/* DTCP_key_dev */
	{ 0x168, 4 },	/* pvt_128key0 */
	{ 0x178, 4 },	/* pvt_128key1 */
	{ 0x188, 4 },	/* pvt_128key2 */
	{ 0x198, 4 },	/* pvt_128key3 */
	{ 0x1a8, 8 },	/* ek */
	{ 0x1c8, 4 },	/* secure_emu_key */
	{ 0x200, 16 },	/* public_key0 */
	{ 0x240, 16 },	/* public_key1 */
	{ 0x280, 16 },	/* boot_info */
};

/* otp_data word index of the later members */
#define SB_DATA_ANTIROLL	13	/* u8 in the low byte */
#define SB_DATA_GP1		14
#define SB_DATA_BOOTMODEDISABLE	15	/* u8 in the low byte */
#define SB_DATA_PREBOOT_DDR_CFG	16
#define SB_DATA_STAGEID		17

static struct {
	u8 otp[SB_OTP_SIZE];
	unsigned int depth;
	unsigned int unbracketed;
	unsigned int pgm_calls;
	int leaky;
	enum sandbox_adi_rom_fault fault;
} sb;

static u32 *sb_reg(ulong base, unsigned int offset)
{
	return map_sysmem(base + offset, sizeof(u32));
}

static void sb_call(void)
{
	if (!sb.depth)
		sb.unbracketed++;
}

static unsigned int sb_count(void)
{
	const u16 *h = (const u16 *)(sb.otp + SB_ANTIROLL);
	unsigned int n = 0;

	while (n < SB_ANTIROLL_MAX && h[n])
		n++;

	return n;
}

/* Program the half-word at @pos */
static bool sb_pgm_half(unsigned int pos, u16 val)
{
	u16 *h = (u16 *)(sb.otp + pos);

	if (!val || *h == val)
		return true;
	if (*h)
		return false;	/* already programmed */
	if ((int)pos != sb.leaky)
		*h = val;	/* a leaky half-word does not take */

	return true;
}

static bool sb_pgm_field(u32 addr, unsigned int offset, unsigned int words)
{
	const u16 *src;
	unsigned int i;
	bool ok = true;

	if (!addr)
		return true;

	src = map_sysmem(addr, words * sizeof(u32));
	for (i = 0; i < words * sizeof(u32) / sizeof(u16); i++)
		ok &= sb_pgm_half(offset + i * sizeof(u16), src[i]);
	unmap_sysmem(src);

	return ok;
}

/* Start a programming call: its injected fault, if any */
static bool sb_pgm_begin(void)
{
	sb_call();
	sb.pgm_calls++;

	if (sb.fault == SANDBOX_ADI_ROM_FAIL) {
		sb.fault = SANDBOX_ADI_ROM_OK;
		return false;
	}
	if (sb.fault == SANDBOX_ADI_ROM_STAT_ERROR) {
		sb.fault = SANDBOX_ADI_ROM_OK;
		*sb_reg(SANDBOX_ADI_SC58X_OTPC, SB_OTPC_STAT) |=
			SB_OTPC_STAT_ADDRERR;
	}

	return true;
}

static bool sb_get(u32 cmd, u32 *data)
{
	sb_call();
	if (cmd != SB_OTPCMD_ANTIROLL)
		return false;

	data[0] = sb_count();
	return true;
}

static bool sb_get_data(u32 *data, u16 offset, u16 words)
{
	sb_call();
	if (offset % sizeof(u32) || offset + words * sizeof(u32) > SB_OTP_SIZE)
		return false;

	memcpy(data, sb.otp + offset, words * sizeof(u32));
	return true;
}

static bool sb_pgm(void *otp_data)
{
	const u32 *w = otp_data;
	unsigned int i, count, cur;
	bool ok = true;

	if (!sb_pgm_begin())
		return false;

	for (i = 0; i < ARRAY_SIZE(sb_fields); i++)
		ok &= sb_pgm_field(w[i], sb_fields[i].offset,
				   sb_fields[i].words);
	ok &= sb_pgm_field(w[SB_DATA_GP1], SB_GP1, SB_GP1_WORDS);
	ok &= sb_pgm_half(SB_BOOTMODEDISABLE, (u8)w[SB_DATA_BOOTMODEDISABLE]);
	ok &= sb_pgm_field(w[SB_DATA_PREBOOT_DDR_CFG], SB_PREBOOT_DDR_CFG,
			   SB_PREBOOT_DDR_CFG_WORDS);
	ok &= sb_pgm_field(w[SB_DATA_STAGEID], SB_STAGEID, SB_STAGEID_WORDS);

	/* The counter: never below its count */
	count = (u8)w[SB_DATA_ANTIROLL];
	cur = sb_count();
	if (count < cur || count > SB_ANTIROLL_MAX)
		return false;
	for (; cur < count; cur++)
		ok &= sb_pgm_half(SB_ANTIROLL + cur * sizeof(u16),
				  SB_ANTIROLL_UNIT);

	return ok;
}

static bool sb_lock(void)
{
	if (!sb_pgm_begin())
		return false;

	return sb_pgm_half(SB_LOCK, SB_LOCK_BIT);
}

static bool sb_cfg(void)
{
	sb_call();
	return true;
}

const struct adi_rom_otp_api adi_sc58x_rom_otp_api = {
	.get		= sb_get,
	.get_data	= sb_get_data,
	.pgm		= sb_pgm,
	.lock		= sb_lock,
	.cfg		= sb_cfg,
};

void adi_rom_enter(void)
{
	sb.depth++;
}

void adi_rom_exit(void)
{
	if (sb.depth)
		sb.depth--;
}

void sandbox_adi_rom_init(void)
{
	static const u8 fsn[SB_FSN_BYTES] = {
		0x46, 0x53, 0x4e, 0x31, 0x02, 0x03, 0x04, 0x05,
		0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
	};

	memset(&sb, 0, sizeof(sb));
	sb.leaky = -1;
	memcpy(sb.otp + SB_FSN, fsn, sizeof(fsn));

	memset(sb_reg(SANDBOX_ADI_SC58X_OTPC, 0), 0, SB_REGS_SIZE);
	*sb_reg(SANDBOX_ADI_SC58X_OTPC, SB_OTPC_CTL) = SB_OTPC_CTL_READMODE;
	*sb_reg(SANDBOX_ADI_SC58X_OTPC, SB_OTPC_PMC_STAT) = SB_PMC_IDLE;

	/* Booted in SPI master mode; core reset and stop always complete */
	memset(sb_reg(SANDBOX_ADI_SC58X_RCU, 0), 0, SB_REGS_SIZE);
	*sb_reg(SANDBOX_ADI_SC58X_RCU, SB_RCU_STAT) =
		FIELD_PREP(SB_RCU_STAT_BMODE, SB_BMODE_SPI_MASTER);
	*sb_reg(SANDBOX_ADI_SC58X_RCU, SB_RCU_SISTAT) = SB_RCU_SI_SHARCS;
	*sb_reg(SANDBOX_ADI_SC58X_RCU, SB_RCU_CRSTAT) =
		SANDBOX_ADI_SC58X_SHARCS;

	/* Some clock settings */
	memset(sb_reg(SANDBOX_ADI_SC58X_CGU0, 0), 0, SB_REGS_SIZE);
	memset(sb_reg(SANDBOX_ADI_SC58X_CGU1, 0), 0, SB_REGS_SIZE);
	*sb_reg(SANDBOX_ADI_SC58X_CGU0, SB_CGU_CTL) =
		FIELD_PREP(SB_CGU_CTL_MSEL, SANDBOX_ADI_SC58X_MSEL);
	*sb_reg(SANDBOX_ADI_SC58X_CGU0, SB_CGU_DIV) =
		FIELD_PREP(SB_CGU_DIV_DSEL, 2) |
		FIELD_PREP(SB_CGU_DIV_SYSSEL, 2) |
		FIELD_PREP(SB_CGU_DIV_CSEL, 1);
}

void sandbox_adi_rom_reset(void)
{
	if (sb.otp[SB_LOCK] & SB_LOCK_BIT)
		*sb_reg(SANDBOX_ADI_SC58X_OTPC, SB_OTPC_SECU_STATE) =
			SB_OTPC_SECU_LOCKED;
	*sb_reg(SANDBOX_ADI_SC58X_RCU, SB_RCU_CRCTL) = 0;
}

void sandbox_adi_rom_inject(int leaky_offset, enum sandbox_adi_rom_fault fault)
{
	sb.leaky = leaky_offset;
	if (fault == SANDBOX_ADI_ROM_PMC_BUSY)
		*sb_reg(SANDBOX_ADI_SC58X_OTPC, SB_OTPC_PMC_STAT) = SB_PMC_BUSY;
	else
		sb.fault = fault;
}

u8 *sandbox_adi_rom_otp(void)
{
	return sb.otp;
}

unsigned int sandbox_adi_rom_pgm_calls(void)
{
	return sb.pgm_calls;
}

unsigned int sandbox_adi_rom_unbracketed_calls(void)
{
	return sb.unbracketed;
}
