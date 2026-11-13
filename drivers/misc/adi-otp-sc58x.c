// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ADI ADSP OTP driver: the ADSP-SC58x family.
 *
 * Everything the SC58x needs (struct adi_otp_soc): its OTP layout, how its
 * boot ROM reads, programs and locks the OTP, its rules for what may be
 * programmed and its silicon anomalies. adi-otp.c parses and checks
 * requests and calls in here.
 *
 * Sources: CCES 3.1.0 defSC58x_rom_jumptable.h, cdefSC58x_rom.h and
 * cdefSC58x_rom_otpLayout.h; the ADSP-SC58x/ADSP-2158x Hardware Reference
 * Manual (OTPC, RCU, CGU and boot ROM chapters) and silicon anomaly list;
 * EE-366 and EE-384.
 *
 * The OTP is only reached through the boot ROM API (include/adi_rom.h),
 * which implements its redundancy and ECC scheme. What the ROM and the
 * silicon need, and where it is handled:
 *  - Anomaly 20000039, the ROM's results do not reliably report OTPC
 *    errors: the OTPC_STAT and PMC_STAT error bits are checked around
 *    every ROM call (adi_sc58x_check_begin/end()).
 *  - The ROM waits for the OTPC's programming controller (PMC) without a
 *    timeout: every programming call is preceded by otp_cfg() and needs
 *    an idle PMC (adi_sc58x_prepare_program()).
 *  - otp_pgm() always also programs the anti-rollback counter, from a
 *    count in otp_data, and fails if that is below the current count: it
 *    is given the current count unless the counter is being advanced
 *    (adi_sc58x_otp_apply()).
 *  - Anomaly 20000084, simultaneous OTP accesses by several cores can
 *    hang a core: the SHARC+ cores are held in reset before programming
 *    (adi_sc58x_hold_sharcs()).
 *  - Anomaly 20000129, programmed bits may not take: each field is read
 *    back before the next is programmed (adi_sc58x_otp_apply()).
 *  - Anomaly 20000108, a locked part cannot read its factory serial
 *    number: locking needs it in gp1 (adi_sc58x_check_lock()), and
 *    ADI_OTP_SC58X_FLAG_FSN_COPY copies it there (adi_sc58x_otp_prepare()).
 *  - Anomaly 20000073, the preboot's DMC setup is limited to a 300 MHz
 *    DCLK: it is never enabled (adi_sc58x_check_dmc()).
 *
 * The rules (adi_sc58x_otp_check()) refuse any request that could leave
 * the part unable to boot, judged on the OTP content after the request.
 * The boot mode, clock and DMC rules can each be turned off with a flag
 * (ADI_OTP_SC58X_FLAG_*_UNCHECKED), for settings checked by other means:
 *  - otpTiming, the OTP read timing override in boot_info word 15, is
 *    never written (a guard): a wrong value makes the OTP, and the boot,
 *    fail.
 *  - bootModeDisable may not disable the boot mode the part boots in, and
 *    the boot command of that mode may not be changed (it cannot be
 *    tested from here). Neither may be written when that mode is unknown
 *    (SYS_BMODE = 0). Unless ADI_OTP_SC58X_FLAG_BOOT_MODE_UNCHECKED.
 *  - Clock settings for the preboot (boot_info CGU fields) must equal the
 *    registers the running system uses, i.e. settings known to work.
 *    Unless ADI_OTP_SC58X_FLAG_CGU_UNCHECKED.
 *  - The preboot DMC configuration may not be enabled or changed while
 *    enabled: it cannot be validated here. Unless
 *    ADI_OTP_SC58X_FLAG_DMC_UNCHECKED.
 *  - Locking needs the key secure boot will use (EE-366, EE-384): the
 *    selected public key, or with decryptOnly the selected private key,
 *    must be programmed; the factory serial number must be in gp1 unless
 *    ADI_OTP_SC58X_FLAG_FSN_IGNORE; and a blank secure_emu_key is refused
 *    unless ADI_OTP_SC58X_FLAG_EMU_KEY_BLANK: the ROM loads it as a debug
 *    key of all zeros, which debuggers use by default (OpenOCD's
 *    USERKEY0-3).
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <adi_rom.h>
#include <dm.h>
#include <errno.h>
#include <mapmem.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/devres.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <u-boot/schedule.h>

#include "adi-otp.h"

/* ------------------------------------------------------------------------
 * OTP layout
 */

/*
 * struct otp_data from cdefSC58x_rom_otpLayout.h, the argument of
 * otp_pgm(): the 32-bit address of the data of each field, except
 * antiroll_nv_cntr and bootModeDisable which are values. "Any pointers
 * left null will not be written, any data values of 0 will not be
 * written." bootModeDisable is an 8-bit bit-field, the low byte of its
 * u32.
 */
struct adi_sc58x_otp_data {
	u32 huk;
	u32 DTCP_key_ecc;
	u32 DTCP_key_cont;
	u32 DTCP_key_dev;
	u32 pvt_128key0;
	u32 pvt_128key1;
	u32 pvt_128key2;
	u32 pvt_128key3;
	u32 ek;
	u32 secure_emu_key;
	u32 public_key0;
	u32 public_key1;
	u32 boot_info;
	u8 antiroll_nv_cntr;
	u32 gp1;
	u32 bootModeDisable;
	u32 preboot_ddr_cfg;
	u32 stageID;
};

static_assert(offsetof(struct adi_sc58x_otp_data, antiroll_nv_cntr) == 52);
static_assert(offsetof(struct adi_sc58x_otp_data, gp1) == 56);
static_assert(offsetof(struct adi_sc58x_otp_data, bootModeDisable) == 60);
static_assert(offsetof(struct adi_sc58x_otp_data, stageID) == 68);
static_assert(sizeof(struct adi_sc58x_otp_data) == 72);

/*
 * The fields: byte offset and size in 32-bit words (ROM_OTP_<name> and
 * ROM_OTP_SZ_<name> in cdefSC58x_rom_otpLayout.h)
 */
#define SC58X_HUK			0x000
#define SC58X_HUK_WORDS			8
#define SC58X_DTCP_KEY_ECC		0x020
#define SC58X_DTCP_KEY_ECC_WORDS	40
#define SC58X_DTCP_KEY_CONT		0x0c0
#define SC58X_DTCP_KEY_CONT_WORDS	10
#define SC58X_DTCP_KEY_DEV		0x0e8
#define SC58X_DTCP_KEY_DEV_WORDS	32
#define SC58X_PVT_128KEY0		0x168
#define SC58X_PVT_128KEY0_WORDS		4
#define SC58X_PVT_128KEY1		0x178
#define SC58X_PVT_128KEY1_WORDS		4
#define SC58X_PVT_128KEY2		0x188
#define SC58X_PVT_128KEY2_WORDS		4
#define SC58X_PVT_128KEY3		0x198
#define SC58X_PVT_128KEY3_WORDS		4
#define SC58X_EK			0x1a8
#define SC58X_EK_WORDS			8
#define SC58X_SECURE_EMU_KEY		0x1c8
#define SC58X_SECURE_EMU_KEY_WORDS	4
#define SC58X_PUBLIC_KEY0		0x200
#define SC58X_PUBLIC_KEY0_WORDS		16
#define SC58X_PUBLIC_KEY1		0x240
#define SC58X_PUBLIC_KEY1_WORDS		16
#define SC58X_BOOT_INFO			0x280
#define SC58X_BOOT_INFO_WORDS		16
#define SC58X_ANTIROLL			0x2c0
#define SC58X_ANTIROLL_WORDS		16
#define SC58X_GP1			0x300
#define SC58X_GP1_WORDS			16
#define SC58X_BOOTMODEDISABLE		0x340
#define SC58X_BOOTMODEDISABLE_WORDS	1
#define SC58X_PREBOOT_DDR_CFG		0x344
#define SC58X_PREBOOT_DDR_CFG_WORDS	12
#define SC58X_STAGEID			0x374
#define SC58X_STAGEID_WORDS		2
#define SC58X_FSN			0x380
#define SC58X_FSN_WORDS			4

#define SC58X_BYTES(words)		((unsigned int)((words) * sizeof(u32)))
#define SC58X_FSN_BYTES			SC58X_BYTES(SC58X_FSN_WORDS)
#define SC58X_SECURE_EMU_KEY_BYTES	SC58X_BYTES(SC58X_SECURE_EMU_KEY_WORDS)
#define SC58X_PREBOOT_DDR_CFG_BYTES	SC58X_BYTES(SC58X_PREBOOT_DDR_CFG_WORDS)
/* The end of the area the driver covers: the end of fsn */
#define SC58X_OTP_SIZE			(SC58X_FSN + SC58X_FSN_BYTES)

/* antiroll_nv_cntr counts up to 32 */
#define SC58X_ANTIROLL_MAX		32
/* bootModeDisable is an 8-bit value */
#define SC58X_BOOTMODEDISABLE_MASK	GENMASK(7, 0)
/* bootModeDisable: the bit that disables boot mode @bmode */
#define SC58X_BMD_BIT(bmode)		BIT((bmode) - 1)
/* stageID is 48 bits: the rest of its second word is reserved */
#define SC58X_STAGEID_BYTES		6

/* HRM OTP layout "lock": otp_lock() programs bit 0 there */
#define SC58X_LOCK			0x48c
#define SC58X_LOCK_WORDS		1
#define SC58X_LOCK_BIT			BIT(0)

/* otp_get() command of the anti-rollback counter: returns its count */
#define SC58X_OTPCMD_ANTIROLL		15

/*
 * boot_info (ADI_ROM_OTP_BOOT_INFO in cdefSC58x_rom.h): its words, and the
 * bit-fields of each
 */
#define SC58X_BI(word)			(SC58X_BOOT_INFO + SC58X_BYTES(word))
/* cgu (ADI_ROM_OTP_BOOT_CGU_INFO): words 0-3 */
#define BI_CGU0				0
#define BI_CGU0_CTL_WEN			BIT(0)
#define BI_CGU0_DIV_WEN			BIT(1)
#define BI_CGU0_DIV_DSEL		GENMASK(7, 3)
#define BI_CGU0_DIV_CSEL		GENMASK(12, 8)
#define BI_CGU0_DIV_S0SEL		GENMASK(15, 13)
#define BI_CGU0_DIV_SYSSEL		GENMASK(20, 16)
#define BI_CGU0_DIV_S1SEL		GENMASK(23, 21)
#define BI_CGU0_DIV_OSEL		GENMASK(30, 24)
#define BI_CGU0_CTL_DF			BIT(31)
#define BI_CGU1				1
#define BI_CGU1_CTL_MSEL		GENMASK(6, 0)
#define BI_CGU1_CLKOUTSEL_USBCLKSEL	GENMASK(13, 8)
#define BI_CGU1_CLKOUTSEL_CLKOUTSEL	GENMASK(18, 14)
#define BI_CGU1_CLKOUTSEL_WEN		BIT(19)
#define BI_CGU2				2
#define BI_CGU2_OSCWCTL0_WEN		BIT(0)
#define BI_CGU2_OSCWCTL0		GENMASK(17, 1)	/* HODF..FAULTPINDIS */
#define BI_CGU2_OSCWCTL1_WEN		BIT(18)
#define BI_CGU2_OSCWCTL1		GENMASK(31, 19)	/* HODF..BOUF */
#define BI_CGU3				3
#define BI_CGU3_OSCWCTL1		GENMASK(3, 0)	/* BOUEN..FAULTPINDIS */
#define BI_CGU_WORDS			4
/* An oscwctl instance: CGU_OSCWDCTL's fields HODF..FAULTPINDIS, packed */
#define BI_OSCW_HODF_TO_MONDIS		GENMASK(15, 0)
#define BI_OSCW_BOUEN_TO_FAULTPINDIS	GENMASK(16, 13)	/* oscwctl1: word 3 */
#define BI_OSCW_FAULTPINDIS		BIT(16)
/* bcmd (ADI_ROM_OTP_BOOT_CMD_INFO): words 4-7 */
#define BI_BCMD_SPI_MASTER		4	/* spiMasterBootCmd */
#define BI_BCMD_SPI_SLAVE		5	/* spiSlaveBootCmd */
#define BI_BCMD_LINKPORT		6	/* lpBootCmd */
#define BI_BCMD_UART			7	/* uartBootCmd */
#define BI_BCMD_WORDS			4
/* bcfg (ADI_ROM_OTP_BOOT_CFG): words 8-13, two flags each */
#define BI_BCFG0			8
#define BI_BCFG0_DECRYPT_ONLY_EN	BIT(16)
#define BI_BCFG1			9
#define BI_BCFG1_DECRYPT_ONLY_INV	BIT(16)
#define BI_BCFG2			10
#define BI_BCFG2_PUBKEY0_INV		BIT(0)
#define BI_BCFG2_PUBKEY1_INV		BIT(16)
#define BI_BCFG3			11
#define BI_BCFG3_PRIVKEY0_INV		BIT(0)
#define BI_BCFG3_PRIVKEY1_INV		BIT(16)
#define BI_BCFG4			12
#define BI_BCFG4_PRIVKEY2_INV		BIT(0)
#define BI_BCFG4_PRIVKEY3_INV		BIT(16)
#define BI_BCFG5			13
#define BI_BCFG5_DMC_EN			BIT(0)
#define BI_BCFG5_DMC_INV		BIT(16)
/* otpReadTiming: the low half of word 15 */
#define BI_OTP_READ_TIMING		15

/**
 * struct adi_sc58x_field - how the boot ROM handles a field
 * (struct adi_otp_field.soc_data; NULL for one it does not program)
 * @otp_data_off: byte offset of the field's member in otp_data
 */
struct adi_sc58x_field {
	u8 otp_data_off;
};

/* The struct adi_sc58x_field of the otp_data member @_name */
#define SC58X_ROM_FIELD(_name)						\
	(&(const struct adi_sc58x_field) {				\
		.otp_data_off = offsetof(struct adi_sc58x_otp_data, _name) })

#define SC58X_FIELD(_name, _NAME, _kind)				\
	{ .name = #_name, .offset = SC58X_##_NAME,			\
	  .words = SC58X_##_NAME##_WORDS, .kind = _kind,		\
	  .soc_data = SC58X_ROM_FIELD(_name) }
#define SC58X_DATA(_name, _NAME)					\
	SC58X_FIELD(_name, _NAME, ADI_OTP_DATA)

/* The customer OTP area (0x000-0x37f) and the factory serial number */
static const struct adi_otp_field adi_sc58x_otp_fields[] = {
	SC58X_DATA(huk,			HUK),
	SC58X_DATA(DTCP_key_ecc,	DTCP_KEY_ECC),
	SC58X_DATA(DTCP_key_cont,	DTCP_KEY_CONT),
	SC58X_DATA(DTCP_key_dev,	DTCP_KEY_DEV),
	SC58X_DATA(pvt_128key0,		PVT_128KEY0),
	SC58X_DATA(pvt_128key1,		PVT_128KEY1),
	SC58X_DATA(pvt_128key2,		PVT_128KEY2),
	SC58X_DATA(pvt_128key3,		PVT_128KEY3),
	SC58X_DATA(ek,			EK),
	SC58X_DATA(secure_emu_key,	SECURE_EMU_KEY),
	/* 0x1d8-0x1ff: reserved */
	SC58X_DATA(public_key0,		PUBLIC_KEY0),
	SC58X_DATA(public_key1,		PUBLIC_KEY1),
	SC58X_DATA(boot_info,		BOOT_INFO),
	{ .name = "antiroll_nv_cntr", .offset = SC58X_ANTIROLL,
	  .words = SC58X_ANTIROLL_WORDS, .kind = ADI_OTP_COUNTER,
	  .counter_max = SC58X_ANTIROLL_MAX,
	  .soc_data = SC58X_ROM_FIELD(antiroll_nv_cntr) },
	SC58X_DATA(gp1,			GP1),
	/*
	 * CCES: the word at 0x340 (ROM_OTP_bootModeDisable), an 8-bit value
	 * in otp_data: the value is the word's low byte, not byte 0x343 as
	 * the HRM's OTP layout table shows.
	 */
	{ .name = "bootModeDisable", .offset = SC58X_BOOTMODEDISABLE,
	  .words = SC58X_BOOTMODEDISABLE_WORDS, .kind = ADI_OTP_VALUE,
	  .val_mask = SC58X_BOOTMODEDISABLE_MASK,
	  .soc_data = SC58X_ROM_FIELD(bootModeDisable) },
	SC58X_DATA(preboot_ddr_cfg,	PREBOOT_DDR_CFG),
	SC58X_DATA(stageID,		STAGEID),
	/* 0x37c-0x37f: reserved */
	/* Factory serial number, in the ADI area: readable on an open part */
	{ .name = "fsn", .offset = SC58X_FSN, .words = SC58X_FSN_WORDS,
	  .kind = ADI_OTP_READ_ONLY },
};

static const struct adi_otp_guard adi_sc58x_otp_guards[] = {
	/*
	 * boot_info word 15 is otpTiming, the OTP read timing override
	 * (ROM_OTP_otpTiming, 0x2bc, in CCES; the HRM's table shows 0x2be):
	 * a wrong value makes the OTP unreadable and the boot fail.
	 */
	{ SC58X_BI(BI_OTP_READ_TIMING), sizeof(u32),
	  "boot_info word 15 holds otpTiming, the OTP read timing override" },
	{ SC58X_STAGEID + SC58X_STAGEID_BYTES, sizeof(u16),
	  "reserved half-word after stageID" },
};

/* ------------------------------------------------------------------------
 * Registers and state
 */

/* Each block's registers that are used here */
#define SC58X_REGS_SIZE		0x100

/* OTPC. 0x30-0x73 is a hole: any access to it aborts. */
#define OTPC_CTL		0x00
#define OTPC_CTL_READMODE	BIT(0)
#define OTPC_STAT		0x04
/* The error bits: OTPC_STAT.ADDRERR and PMC_STAT[5:4] */
#define OTPC_STAT_ERRORS	GENMASK(14, 13)
#define OTPC_SECU_STATE		0x2c
#define OTPC_SECU_PARTLOCK	GENMASK(1, 0)
#define OTPC_SECU_OPEN		0
#define OTPC_SECU_LOCKED	1
#define OTPC_SECU_UNLOCKED	2
/* PMC registers: only accessible while OTPC_CTL.READMODE = 0 */
#define OTPC_PMC_STAT		0x84
#define OTPC_PMC_STAT_STATE	GENMASK(7, 6)
#define OTPC_PMC_STATE_IDLE	2
#define OTPC_PMC_STAT_ERRORS	GENMASK(5, 4)

/* RCU (from the adi,rcu phandle) */
#define RCU_STAT		0x04
#define RCU_STAT_BMODE		GENMASK(11, 8)
/* RCU_STAT.BMODE (SYS_BMODE): no boot mode, and those with a boot command */
#define SC58X_BMODE_NONE	0
#define SC58X_BMODE_SPI_MASTER	1
#define SC58X_BMODE_SPI_SLAVE	2
#define SC58X_BMODE_LINKPORT	6
#define SC58X_BMODE_UART	7
#define RCU_CRCTL		0x08
#define RCU_CRSTAT		0x0c
#define RCU_SIDIS		0x10
#define RCU_SISTAT		0x14
#define RCU_SVECT(n)		(0x20 + 4 * (n))
/* Core @n's bit: in CRCTL and CRSTAT, and in SIDIS and SISTAT */
#define RCU_CR_CORE(n)		BIT(n)
#define RCU_SI_CORE(n)		BIT((n) - 1)
#define SC58X_SHARC_FIRST	1
#define SC58X_SHARC_LAST	2
#define SC58X_RCU_TIMEOUT_US	10000

/* CGU0 and CGU1 (the "cgu0" and "cgu1" registers of the clock node) */
#define CGU_CTL			0x00
#define CGU_CTL_DF		BIT(0)
#define CGU_CTL_MSEL		GENMASK(14, 8)
#define CGU_DIV			0x0c
#define CGU_DIV_CSEL		GENMASK(4, 0)
#define CGU_DIV_S0SEL		GENMASK(7, 5)
#define CGU_DIV_SYSSEL		GENMASK(12, 8)
#define CGU_DIV_S1SEL		GENMASK(15, 13)
#define CGU_DIV_DSEL		GENMASK(20, 16)
#define CGU_DIV_OSEL		GENMASK(28, 22)
#define CGU_CLKOUTSEL		0x10
#define CGU_CLKOUTSEL_CLKOUTSEL	GENMASK(4, 0)
#define CGU_CLKOUTSEL_USBCLKSEL	GENMASK(21, 16)
#define CGU_OSCWDCTL		0x14
#define CGU_OSCWDCTL_HODF_TO_MONDIS GENMASK(15, 0)
#define CGU_OSCWDCTL_FAULTPINDIS BIT(23)
/* The bits of each register that the boot_info CGU fields set */
#define CGU_CTL_OTP_MASK	(CGU_CTL_MSEL | CGU_CTL_DF)
#define CGU_DIV_OTP_MASK	(CGU_DIV_OSEL | CGU_DIV_DSEL | CGU_DIV_S1SEL | \
				 CGU_DIV_SYSSEL | CGU_DIV_S0SEL | CGU_DIV_CSEL)
#define CGU_CLKOUTSEL_OTP_MASK	(CGU_CLKOUTSEL_USBCLKSEL | \
				 CGU_CLKOUTSEL_CLKOUTSEL)
#define CGU_OSCWDCTL_OTP_MASK	(CGU_OSCWDCTL_FAULTPINDIS | \
				 CGU_OSCWDCTL_HODF_TO_MONDIS)

/**
 * struct adi_sc58x_soc - the ADSP-SC58x family
 * @soc: what adi-otp.c sees. It comes first: adi-otp.c's compatible table
 *	points at the whole structure, and uses that as a pointer to @soc.
 * @rom: the boot ROM's OTP functions
 */
struct adi_sc58x_soc {
	struct adi_otp_soc soc;
	const struct adi_rom_otp_api *rom;
};

static_assert(offsetof(struct adi_sc58x_soc, soc) == 0);

static const struct adi_sc58x_soc *to_sc58x_soc(struct udevice *dev)
{
	return container_of(adi_otp_get_soc(dev), struct adi_sc58x_soc, soc);
}

/**
 * struct adi_sc58x_otp - SoC state
 * @otpc: OTPC registers
 * @rcu: RCU registers
 * @cgu0: CGU0 registers
 * @cgu1: CGU1 registers
 * @otp_data: the argument of otp_pgm()
 * @rom_buf: what the ROM reads, before it goes where it was asked for; as
 *	large as the largest field (otp_get() may fill a whole one)
 * @rom_buf_words: size of @rom_buf in 32-bit words
 *
 * What is handed to the ROM is on the heap, never the stack.
 */
struct adi_sc58x_otp {
	void __iomem *otpc;
	void __iomem *rcu;
	void __iomem *cgu0;
	void __iomem *cgu1;
	struct adi_sc58x_otp_data *otp_data;
	u32 *rom_buf;
	unsigned int rom_buf_words;
};

static struct adi_sc58x_otp *to_sc58x(struct udevice *dev)
{
	return adi_otp_soc_priv(dev);
}

static u32 adi_sc58x_otpc_stat(struct adi_sc58x_otp *state)
{
	return readl(state->otpc + OTPC_STAT);
}

static u32 adi_sc58x_partlock(struct adi_sc58x_otp *state)
{
	return FIELD_GET(OTPC_SECU_PARTLOCK,
			 readl(state->otpc + OTPC_SECU_STATE));
}

/* PMC_STAT, read with READMODE cleared, as the PMC registers need */
static u32 adi_sc58x_pmc_stat(struct adi_sc58x_otp *state)
{
	u32 ctl, stat;

	ctl = readl(state->otpc + OTPC_CTL);
	writel(ctl & ~OTPC_CTL_READMODE, state->otpc + OTPC_CTL);
	readl(state->otpc + OTPC_CTL);

	stat = readl(state->otpc + OTPC_PMC_STAT);

	writel(ctl, state->otpc + OTPC_CTL);
	readl(state->otpc + OTPC_CTL);

	return stat;
}

/* ------------------------------------------------------------------------
 * Boot ROM calls
 *
 * One function per ROM function. Each makes the ROM executable for the
 * call and checks the OTPC error bits around it (anomaly 20000039).
 * @target names what the call works on, for diagnostics.
 */

/**
 * struct adi_sc58x_check - the OTPC error bits before a ROM call
 * @otpc_stat: OTPC_STAT
 * @pmc_stat: PMC_STAT, for a programming call
 * @programming: the call programs the OTP: check PMC_STAT too
 */
struct adi_sc58x_check {
	u32 otpc_stat;
	u32 pmc_stat;
	bool programming;
};

static void adi_sc58x_check_begin(struct adi_sc58x_otp *state,
				  struct adi_sc58x_check *check)
{
	check->otpc_stat = adi_sc58x_otpc_stat(state);
	check->programming = false;
}

/*
 * Fail if an error bit is set after the call that was clear before it.
 * One that was already set cannot be attributed to the call, and the bits
 * are read-only, so it is only reported.
 */
static int adi_sc58x_check_bits(struct udevice *dev, const char *reg,
				u32 before, u32 after, u32 errors,
				const char *action, const char *target)
{
	if (after & errors & ~before) {
		dev_err(dev, "%s %s: %s reports an error (0x%08x)\n", action,
			target, reg, after);
		return -EIO;
	}
	if (after & errors)
		dev_warn(dev, "%s %s: %s error bits already set (0x%08x), cannot check them\n",
			 action, target, reg, after);

	return 0;
}

/* The outcome of a ROM call: what it returned, and the error bits */
static int adi_sc58x_check_end(struct udevice *dev,
			       const struct adi_sc58x_check *check,
			       bool rom_ok, const char *action,
			       const char *target)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	int ret;

	ret = adi_sc58x_check_bits(dev, "OTPC_STAT", check->otpc_stat,
				   adi_sc58x_otpc_stat(state), OTPC_STAT_ERRORS,
				   action, target);
	if (!ret && check->programming)
		ret = adi_sc58x_check_bits(dev, "PMC_STAT", check->pmc_stat,
					   adi_sc58x_pmc_stat(state),
					   OTPC_PMC_STAT_ERRORS, action,
					   target);
	if (!rom_ok) {
		dev_err(dev, "%s %s: the boot ROM reports a failure\n", action,
			target);
		return -EIO;
	}

	return ret;
}

/* otp_get(): the ROM's view of the field @cmd */
static int adi_sc58x_rom_get(struct udevice *dev, const char *target,
			     u32 cmd, u32 *data)
{
	const struct adi_rom_otp_api *rom = to_sc58x_soc(dev)->rom;
	struct adi_sc58x_otp *state = to_sc58x(dev);
	struct adi_sc58x_check check;
	bool ok;

	adi_rom_enter();
	adi_sc58x_check_begin(state, &check);
	ok = rom->get(cmd, data);
	adi_rom_exit();

	return adi_sc58x_check_end(dev, &check, ok, "reading", target);
}

/* otp_get_data(): @words raw words at byte @offset */
static int adi_sc58x_rom_get_data(struct udevice *dev, const char *target,
				  u32 *data, u16 offset, u16 words)
{
	const struct adi_rom_otp_api *rom = to_sc58x_soc(dev)->rom;
	struct adi_sc58x_otp *state = to_sc58x(dev);
	struct adi_sc58x_check check;
	bool ok;

	adi_rom_enter();
	adi_sc58x_check_begin(state, &check);
	ok = rom->get_data(data, offset, words);
	adi_rom_exit();

	return adi_sc58x_check_end(dev, &check, ok, "reading", target);
}

/*
 * Before a programming call, with the ROM executable: reconfigure the
 * OTPC with otp_cfg() and require an idle PMC, since the ROM waits for the
 * PMC without a timeout; then take the error bits for the check.
 */
static int adi_sc58x_prepare_program(struct udevice *dev,
				     struct adi_sc58x_check *check,
				     const char *action, const char *target)
{
	const struct adi_rom_otp_api *rom = to_sc58x_soc(dev)->rom;
	struct adi_sc58x_otp *state = to_sc58x(dev);
	bool ok;
	int ret;

	adi_sc58x_check_begin(state, check);
	ok = rom->cfg();
	ret = adi_sc58x_check_end(dev, check, ok, "configuring the OTPC for",
				  target);
	if (ret)
		return ret;

	check->pmc_stat = adi_sc58x_pmc_stat(state);
	if (FIELD_GET(OTPC_PMC_STAT_STATE, check->pmc_stat) !=
	    OTPC_PMC_STATE_IDLE) {
		dev_err(dev, "%s %s: PMC not idle (PMC_STAT 0x%08x)\n", action,
			target, check->pmc_stat);
		return -EIO;
	}
	check->otpc_stat = adi_sc58x_otpc_stat(state);
	check->programming = true;

	return 0;
}

/* otp_pgm() of state->otp_data, which the caller has filled in */
static int adi_sc58x_rom_pgm(struct udevice *dev, const char *target)
{
	const struct adi_rom_otp_api *rom = to_sc58x_soc(dev)->rom;
	struct adi_sc58x_otp *state = to_sc58x(dev);
	struct adi_sc58x_check check;
	bool ok = false;
	int ret;

	adi_rom_enter();
	ret = adi_sc58x_prepare_program(dev, &check, "programming", target);
	if (!ret)
		ok = rom->pgm(state->otp_data);
	adi_rom_exit();
	if (ret)
		return ret;

	return adi_sc58x_check_end(dev, &check, ok, "programming", target);
}

/* otp_lock() */
static int adi_sc58x_rom_lock(struct udevice *dev)
{
	const struct adi_rom_otp_api *rom = to_sc58x_soc(dev)->rom;
	struct adi_sc58x_check check;
	bool ok = false;
	int ret;

	adi_rom_enter();
	ret = adi_sc58x_prepare_program(dev, &check, "locking", "the part");
	if (!ret)
		ok = rom->lock();
	adi_rom_exit();
	if (ret)
		return ret;

	return adi_sc58x_check_end(dev, &check, ok, "locking", "the part");
}

/* ------------------------------------------------------------------------
 * Reading
 */

static int adi_sc58x_otp_read(struct udevice *dev, unsigned int offset,
			      u32 *data, unsigned int words)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	const struct adi_otp_field *f =
		adi_otp_field_at(adi_otp_get_soc(dev), offset);
	u32 partlock = adi_sc58x_partlock(state);
	int ret;

	/*
	 * An "unlocked" part reads zeros for the customer fields, and only
	 * an open part can read the FSN in the ADI area
	 */
	if (partlock == OTPC_SECU_UNLOCKED ||
	    (offset >= SC58X_FSN && partlock != OTPC_SECU_OPEN)) {
		memset(data, 0, words * sizeof(u32));
		return 0;
	}

	/* Through rom_buf: @data may be anywhere, e.g. on the stack */
	if (words > state->rom_buf_words)
		return -EINVAL;
	ret = adi_sc58x_rom_get_data(dev, f ? f->name : "the OTP",
				     state->rom_buf, offset, words);
	if (!ret)
		memcpy(data, state->rom_buf, words * sizeof(u32));
	/* May hold key material */
	memset(state->rom_buf, 0, words * sizeof(u32));

	return ret;
}

/* The ROM's decoded count of the anti-rollback counter */
static int adi_sc58x_otp_read_counter(struct udevice *dev,
				      const struct adi_otp_field *counter,
				      u32 *count)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	int ret;

	memset(state->rom_buf, 0, state->rom_buf_words * sizeof(u32));
	ret = adi_sc58x_rom_get(dev, counter->name, SC58X_OTPCMD_ANTIROLL,
				state->rom_buf);
	*count = state->rom_buf[0];
	if (!ret && *count > counter->counter_max) {
		dev_err(dev, "%s reads %u, more than %u\n", counter->name,
			*count, counter->counter_max);
		ret = -EIO;
	}

	return ret;
}

/*
 * OTPC_SECU_STATE only changes at the next reset after otp_lock(), so a
 * pending lock is recognised from the lock word itself.
 */
static int adi_sc58x_otp_lock_state(struct udevice *dev)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	u32 partlock = adi_sc58x_partlock(state);
	int ret;

	switch (partlock) {
	case OTPC_SECU_OPEN:
		break;
	case OTPC_SECU_LOCKED:
		return ADI_OTP_LOCKED;
	case OTPC_SECU_UNLOCKED:
		return ADI_OTP_UNLOCKED;
	default:
		dev_err(dev, "unknown PARTLOCK state %u\n", partlock);
		return -EIO;
	}

	state->rom_buf[0] = 0;
	ret = adi_sc58x_rom_get_data(dev, "the lock word", state->rom_buf,
				     SC58X_LOCK, SC58X_LOCK_WORDS);
	if (ret)
		return ret;

	return state->rom_buf[0] & SC58X_LOCK_BIT ? ADI_OTP_LOCK_PENDING :
						 ADI_OTP_OPEN;
}

/* ------------------------------------------------------------------------
 * Factory serial number (anomaly 20000108)
 */

static bool adi_sc58x_blank(const u32 *image, unsigned int offset,
			    unsigned int bytes)
{
	return !memchr_inv((const u8 *)image + offset, 0, bytes);
}

/* Whether gp1 of @image holds the 16-byte @fsn at a word offset */
static bool adi_sc58x_gp1_has_fsn(const u32 *image, const u32 *fsn)
{
	unsigned int i;

	for (i = 0; i + SC58X_FSN_WORDS <= SC58X_GP1_WORDS; i++)
		if (!memcmp(image + SC58X_GP1 / sizeof(u32) + i, fsn,
			    SC58X_FSN_BYTES))
			return true;

	return false;
}

/*
 * ADI_OTP_SC58X_FLAG_FSN_COPY: add the FSN to the request, in a blank
 * slot of gp1. It is then checked like any other data.
 */
static int adi_sc58x_otp_prepare(struct udevice *dev,
				 struct adi_otp_request *req)
{
	const u32 *fsn = req->cur + SC58X_FSN / sizeof(u32);
	u32 gp1[SC58X_GP1_WORDS];
	unsigned int i, slot;

	if ((req->flags & ADI_OTP_SC58X_FLAG_FSN_COPY) &&
	    (req->flags & ADI_OTP_SC58X_FLAG_FSN_IGNORE)) {
		dev_err(dev, "the FSN copy and FSN ignore flags are mutually exclusive\n");
		return -EINVAL;
	}
	if (!(req->flags & ADI_OTP_SC58X_FLAG_FSN_COPY))
		return 0;

	if (adi_sc58x_blank(req->cur, SC58X_FSN, SC58X_FSN_BYTES)) {
		dev_err(dev, "cannot copy the factory serial number: it reads blank\n");
		return -EINVAL;
	}

	/* gp1 as it will be: current content plus the requested data */
	for (i = 0; i < SC58X_GP1_WORDS; i++)
		gp1[i] = req->cur[SC58X_GP1 / sizeof(u32) + i] |
			 req->want[SC58X_GP1 / sizeof(u32) + i];

	for (i = 0; i + SC58X_FSN_WORDS <= SC58X_GP1_WORDS; i++) {
		if (!memcmp(gp1 + i, fsn, SC58X_FSN_BYTES)) {
			printf("gp1 already holds the factory serial number at 0x%03x\n",
			       SC58X_GP1 + SC58X_BYTES(i));
			return 0;
		}
	}

	/* The first blank 16-byte slot */
	for (slot = 0; slot < SC58X_GP1_WORDS; slot += SC58X_FSN_WORDS)
		if (!memchr_inv(gp1 + slot, 0, SC58X_FSN_BYTES))
			break;
	if (slot >= SC58X_GP1_WORDS) {
		dev_err(dev, "cannot copy the factory serial number: no blank 16-byte slot in gp1\n");
		return -ENOSPC;
	}

	printf("Copying the factory serial number to gp1 at 0x%03x\n",
	       SC58X_GP1 + SC58X_BYTES(slot));
	return adi_otp_request_add(dev, req, SC58X_GP1 + SC58X_BYTES(slot), fsn,
				   SC58X_FSN_BYTES);
}

/* ------------------------------------------------------------------------
 * Rules: the part must still boot, and the conditions for locking
 */

/* boot_info word @n of the content after the request */
static u32 bi(const struct adi_otp_request *req, unsigned int n)
{
	return adi_otp_word(req->merged, SC58X_BI(n));
}

/* SYS_BMODE value -> boot_info word with its boot command, or 0 */
static unsigned int adi_sc58x_bcmd_word(unsigned int bmode)
{
	switch (bmode) {
	case SC58X_BMODE_SPI_MASTER: return BI_BCMD_SPI_MASTER;
	case SC58X_BMODE_SPI_SLAVE: return BI_BCMD_SPI_SLAVE;
	case SC58X_BMODE_LINKPORT: return BI_BCMD_LINKPORT;
	case SC58X_BMODE_UART: return BI_BCMD_UART;
	default: return 0;
	}
}

static int adi_sc58x_check_boot_mode(struct udevice *dev,
				     const struct adi_otp_request *req)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	unsigned int bmode = FIELD_GET(RCU_STAT_BMODE,
				       readl(state->rcu + RCU_STAT));
	unsigned int word = adi_sc58x_bcmd_word(bmode);
	bool bmd = adi_otp_request_programs(req, SC58X_BOOTMODEDISABLE,
					    sizeof(u32));
	bool bcmd = adi_otp_request_programs(req, SC58X_BI(BI_BCMD_SPI_MASTER),
					     SC58X_BYTES(BI_BCMD_WORDS));

	if (!bmd && !bcmd)
		return 0;

	if (bmode == SC58X_BMODE_NONE) {
		dev_err(dev, "the part runs without a boot mode (SYS_BMODE = 0): bootModeDisable and the boot commands cannot be checked, so they are not written\n");
		return -EPERM;
	}

	if (bmd && (adi_otp_word(req->merged, SC58X_BOOTMODEDISABLE) &
		    SC58X_BMD_BIT(bmode))) {
		dev_err(dev, "bootModeDisable would disable boot mode %u, the one this part boots in\n",
			bmode);
		return -EPERM;
	}

	/*
	 * A boot command replaces the ROM's default for its boot mode; one
	 * that does not work cannot be undone. It cannot be tested here.
	 */
	if (word &&
	    adi_otp_request_programs(req, SC58X_BI(word), sizeof(u32))) {
		dev_err(dev, "the boot command of boot mode %u, the one this part boots in, cannot be validated, so it is not written\n",
			bmode);
		return -EPERM;
	}

	return 0;
}

/*
 * The boot_info CGU fields (ADI_ROM_OTP_BOOT_CGU_INFO, cdefSC58x_rom.h) as
 * register values: the field @bi_field of boot_info word @word moved to
 * the register field @cgu_field
 */
#define BI_TO_CGU(cgu_field, bi_field, word)				\
	FIELD_PREP(cgu_field, FIELD_GET(bi_field, word))

/* CGU_DIV from boot_info word 0 */
static u32 adi_sc58x_cgu_div(u32 w0)
{
	return BI_TO_CGU(CGU_DIV_OSEL, BI_CGU0_DIV_OSEL, w0) |
	       BI_TO_CGU(CGU_DIV_DSEL, BI_CGU0_DIV_DSEL, w0) |
	       BI_TO_CGU(CGU_DIV_S1SEL, BI_CGU0_DIV_S1SEL, w0) |
	       BI_TO_CGU(CGU_DIV_SYSSEL, BI_CGU0_DIV_SYSSEL, w0) |
	       BI_TO_CGU(CGU_DIV_S0SEL, BI_CGU0_DIV_S0SEL, w0) |
	       BI_TO_CGU(CGU_DIV_CSEL, BI_CGU0_DIV_CSEL, w0);
}

/* CGU_CTL from boot_info words 0 and 1 */
static u32 adi_sc58x_cgu_ctl(u32 w0, u32 w1)
{
	return BI_TO_CGU(CGU_CTL_MSEL, BI_CGU1_CTL_MSEL, w1) |
	       BI_TO_CGU(CGU_CTL_DF, BI_CGU0_CTL_DF, w0);
}

/* CGU_CLKOUTSEL from boot_info word 1 */
static u32 adi_sc58x_cgu_clkoutsel(u32 w1)
{
	return BI_TO_CGU(CGU_CLKOUTSEL_USBCLKSEL,
			 BI_CGU1_CLKOUTSEL_USBCLKSEL, w1) |
	       BI_TO_CGU(CGU_CLKOUTSEL_CLKOUTSEL,
			 BI_CGU1_CLKOUTSEL_CLKOUTSEL, w1);
}

/*
 * CGU_OSCWDCTL from the 17 OTP bits of one instance (BI_OSCW_*): HODF[5:0],
 * HODEN, CNGEN, BOUF[4:0], BOUEN, FAULTEN and MONDIS where the register
 * has them, then FAULTPINDIS.
 */
static u32 adi_sc58x_cgu_oscwdctl(u32 v)
{
	return BI_TO_CGU(CGU_OSCWDCTL_HODF_TO_MONDIS,
			 BI_OSCW_HODF_TO_MONDIS, v) |
	       BI_TO_CGU(CGU_OSCWDCTL_FAULTPINDIS, BI_OSCW_FAULTPINDIS, v);
}

static int adi_sc58x_cmp_cgu(struct udevice *dev, const char *reg, u32 otp,
			     void __iomem *addr, u32 mask)
{
	u32 live = readl(addr);

	if ((otp & mask) == (live & mask))
		return 0;

	dev_err(dev, "boot_info would set %s to 0x%08x, but the running system uses 0x%08x (mask 0x%08x): only settings known to work are written\n",
		reg, otp & mask, live & mask, mask);
	return -EPERM;
}

/*
 * The preboot applies the CGU settings of boot_info words 0-3 on every
 * boot. Settings that do not work cannot be undone, so only accept
 * those that the running system uses.
 */
static int adi_sc58x_check_cgu(struct udevice *dev,
			       const struct adi_otp_request *req)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	u32 w0 = bi(req, BI_CGU0), w1 = bi(req, BI_CGU1);
	u32 w2 = bi(req, BI_CGU2), w3 = bi(req, BI_CGU3);
	/* The oscwctl instances; oscwctl1 spans words 2 and 3 */
	u32 oscw0 = FIELD_GET(BI_CGU2_OSCWCTL0, w2);
	u32 oscw1 = FIELD_GET(BI_CGU2_OSCWCTL1, w2) |
		    FIELD_PREP(BI_OSCW_BOUEN_TO_FAULTPINDIS,
			       FIELD_GET(BI_CGU3_OSCWCTL1, w3));
	int ret = 0;

	if (!adi_otp_request_programs(req, SC58X_BI(BI_CGU0),
				      SC58X_BYTES(BI_CGU_WORDS)))
		return 0;

	if (w0 & BI_CGU0_CTL_WEN)
		ret = ret ? : adi_sc58x_cmp_cgu(dev, "CGU0_CTL",
						adi_sc58x_cgu_ctl(w0, w1),
						state->cgu0 + CGU_CTL,
						CGU_CTL_OTP_MASK);
	if (w0 & BI_CGU0_DIV_WEN)
		ret = ret ? : adi_sc58x_cmp_cgu(dev, "CGU0_DIV",
						adi_sc58x_cgu_div(w0),
						state->cgu0 + CGU_DIV,
						CGU_DIV_OTP_MASK);
	if (w1 & BI_CGU1_CLKOUTSEL_WEN)
		ret = ret ? : adi_sc58x_cmp_cgu(dev, "CGU0_CLKOUTSEL",
						adi_sc58x_cgu_clkoutsel(w1),
						state->cgu0 + CGU_CLKOUTSEL,
						CGU_CLKOUTSEL_OTP_MASK);
	if (w2 & BI_CGU2_OSCWCTL0_WEN)
		ret = ret ? : adi_sc58x_cmp_cgu(dev, "CGU0_OSCWDCTL",
						adi_sc58x_cgu_oscwdctl(oscw0),
						state->cgu0 + CGU_OSCWDCTL,
						CGU_OSCWDCTL_OTP_MASK);
	if (w2 & BI_CGU2_OSCWCTL1_WEN)
		ret = ret ? : adi_sc58x_cmp_cgu(dev, "CGU1_OSCWDCTL",
						adi_sc58x_cgu_oscwdctl(oscw1),
						state->cgu1 + CGU_OSCWDCTL,
						CGU_OSCWDCTL_OTP_MASK);

	return ret;
}

/* The preboot DMC configuration (boot_info word 13: dmcEn, dmcInv) */
static int adi_sc58x_check_dmc(struct udevice *dev,
			       const struct adi_otp_request *req)
{
	u32 bcfg5 = bi(req, BI_BCFG5);
	bool enabled = (bcfg5 & BI_BCFG5_DMC_EN) && !(bcfg5 & BI_BCFG5_DMC_INV);
	bool cfg = adi_otp_request_programs(req, SC58X_PREBOOT_DDR_CFG,
					    SC58X_PREBOOT_DDR_CFG_BYTES);

	/* dmcEn is in the low half-word of its word */
	if (enabled && (cfg || adi_otp_request_programs(req, SC58X_BI(BI_BCFG5),
							sizeof(u16)))) {
		dev_err(dev, "the preboot DMC configuration cannot be validated (and is limited to a 300 MHz DCLK, anomaly 20000073), so it is not enabled or changed while enabled\n");
		return -EPERM;
	}
	if (cfg)
		printf("Note: preboot_ddr_cfg is only used once boot_info.dmcEn is set (anomaly 20000073: DCLK up to 300 MHz)\n");

	return 0;
}

/*
 * The key secure boot of a locked part uses: the first one not
 * invalidated in boot_info (EE-366, EE-384). Returns its offset, or 0.
 */
static unsigned int adi_sc58x_auth_key(const struct adi_otp_request *req,
				       bool decrypt_only, const char **name)
{
	/* Each key, with the boot_info flag that invalidates it */
	static const struct {
		unsigned int word;
		u32 inv;
		unsigned int offset;
		const char *name;
	} pub[] = {
		{ BI_BCFG2, BI_BCFG2_PUBKEY0_INV, SC58X_PUBLIC_KEY0, "public_key0" },
		{ BI_BCFG2, BI_BCFG2_PUBKEY1_INV, SC58X_PUBLIC_KEY1, "public_key1" },
	}, pvt[] = {
		{ BI_BCFG3, BI_BCFG3_PRIVKEY0_INV, SC58X_PVT_128KEY0, "pvt_128key0" },
		{ BI_BCFG3, BI_BCFG3_PRIVKEY1_INV, SC58X_PVT_128KEY1, "pvt_128key1" },
		{ BI_BCFG4, BI_BCFG4_PRIVKEY2_INV, SC58X_PVT_128KEY2, "pvt_128key2" },
		{ BI_BCFG4, BI_BCFG4_PRIVKEY3_INV, SC58X_PVT_128KEY3, "pvt_128key3" },
	};
	unsigned int i;

	if (!decrypt_only) {
		for (i = 0; i < ARRAY_SIZE(pub); i++) {
			if (!(bi(req, pub[i].word) & pub[i].inv)) {
				*name = pub[i].name;
				return pub[i].offset;
			}
		}
	} else {
		for (i = 0; i < ARRAY_SIZE(pvt); i++) {
			if (!(bi(req, pvt[i].word) & pvt[i].inv)) {
				*name = pvt[i].name;
				return pvt[i].offset;
			}
		}
	}

	*name = decrypt_only ? "private key" : "public key";
	return 0;
}

static int adi_sc58x_check_lock(struct udevice *dev,
				const struct adi_otp_request *req)
{
	const u32 *fsn = req->cur + SC58X_FSN / sizeof(u32);
	/* decryptOnlyEn, unless decryptOnlyInv */
	bool decrypt_only = (bi(req, BI_BCFG0) & BI_BCFG0_DECRYPT_ONLY_EN) &&
			    !(bi(req, BI_BCFG1) & BI_BCFG1_DECRYPT_ONLY_INV);
	unsigned int key, bytes;
	const char *name;

	key = adi_sc58x_auth_key(req, decrypt_only, &name);
	if (!key) {
		dev_err(dev, "refusing to lock: every %s is invalidated in boot_info\n",
			name);
		return -EPERM;
	}
	bytes = adi_otp_field_bytes(adi_otp_field_at(adi_otp_get_soc(dev),
						     key));
	if (adi_sc58x_blank(req->merged, key, bytes)) {
		dev_err(dev, "refusing to lock: %s, the key secure boot would use, is blank\n",
			name);
		return -EPERM;
	}

	if (!(req->flags & ADI_OTP_SC58X_FLAG_FSN_IGNORE)) {
		if (adi_sc58x_blank(req->cur, SC58X_FSN, SC58X_FSN_BYTES)) {
			dev_err(dev, "refusing to lock: the factory serial number reads blank (set the FSN ignore flag to lock anyway)\n");
			return -EPERM;
		}
		if (!adi_sc58x_gp1_has_fsn(req->merged, fsn)) {
			dev_err(dev, "refusing to lock: gp1 does not hold the factory serial number, which a locked part cannot read (anomaly 20000108); set the FSN copy or FSN ignore flag\n");
			return -EPERM;
		}
	}

	/*
	 * The ROM loads secure_emu_key into TAPC_SDBGKEY0-3 unless it is all
	 * ones (EE-384). A blank one is a debug key of all zeros, the
	 * debuggers' default (OpenOCD's USERKEY0-3 are 0).
	 */
	if (adi_sc58x_blank(req->merged, SC58X_SECURE_EMU_KEY,
			    SC58X_SECURE_EMU_KEY_BYTES)) {
		if (!(req->flags & ADI_OTP_SC58X_FLAG_EMU_KEY_BLANK)) {
			dev_err(dev, "refusing to lock: secure_emu_key is blank, so the debug key is all zeros, a debugger's default: anyone could debug the locked part; program a key, or set the blank debug key flag to lock anyway\n");
			return -EPERM;
		}
		printf("  Warning: secure_emu_key is blank: the debug key is all zeros, a debugger's default, so anyone can debug the locked part\n");
	} else if (!memchr_inv((const u8 *)req->merged + SC58X_SECURE_EMU_KEY,
			       0xff, SC58X_SECURE_EMU_KEY_BYTES)) {
		printf("  Warning: secure_emu_key is all ones: the boot ROM loads no debug key, so the locked part has no debug access unless the application loads one\n");
	}

	printf("Locking: secure boot will authenticate with %s%s\n", name,
	       decrypt_only ? " (decryptOnly: no signature check)" : "");
	printf("  Only secure boot streams for this key will boot, without init blocks; this is not checked here.\n");

	if (req->counter && req->new_count)
		printf("  Note: antiroll_nv_cntr will be %u: boot streams need a rollback ID of at least %u\n",
		       req->new_count, req->new_count);

	return 0;
}

static int adi_sc58x_otp_check(struct udevice *dev,
			       const struct adi_otp_request *req)
{
	int ret = 0;

	if (!(req->flags & ADI_OTP_SC58X_FLAG_BOOT_MODE_UNCHECKED))
		ret = adi_sc58x_check_boot_mode(dev, req);
	if (!ret && !(req->flags & ADI_OTP_SC58X_FLAG_CGU_UNCHECKED))
		ret = adi_sc58x_check_cgu(dev, req);
	if (!ret && !(req->flags & ADI_OTP_SC58X_FLAG_DMC_UNCHECKED))
		ret = adi_sc58x_check_dmc(dev, req);
	if (!ret && (req->flags & ADI_OTP_BUF_FLAG_LOCK))
		ret = adi_sc58x_check_lock(dev, req);

	return ret;
}

/* ------------------------------------------------------------------------
 * SHARC+ cores (anomaly 20000084)
 */

/* Hold SHARC+ core @n in reset: the HRM's core reset sequence, steps 1-7 */
static int adi_sc58x_hold_core(struct udevice *dev, unsigned int n)
{
	void __iomem *rcu = to_sc58x(dev)->rcu;
	u32 val;
	int ret;

	if ((readl(rcu + RCU_CRCTL) & RCU_CR_CORE(n)) &&
	    (readl(rcu + RCU_CRSTAT) & RCU_CR_CORE(n)))
		return 0;	/* already held in reset */

	printf("Holding SHARC+ core %u in reset (RCU_SVECT%u = 0x%08x)\n", n, n,
	       readl(rcu + RCU_SVECT(n)));

	writel(RCU_CR_CORE(n), rcu + RCU_CRSTAT);	/* W1C */
	setbits_le32(rcu + RCU_SIDIS, RCU_SI_CORE(n));
	ret = readl_poll_timeout(rcu + RCU_SISTAT, val, val & RCU_SI_CORE(n),
				 SC58X_RCU_TIMEOUT_US);
	if (ret) {
		clrbits_le32(rcu + RCU_SIDIS, RCU_SI_CORE(n));
		dev_err(dev, "SHARC+ core %u: system interface does not stop\n",
			n);
		return ret;
	}

	setbits_le32(rcu + RCU_CRCTL, RCU_CR_CORE(n));
	ret = readl_poll_timeout(rcu + RCU_CRSTAT, val, val & RCU_CR_CORE(n),
				 SC58X_RCU_TIMEOUT_US);
	clrbits_le32(rcu + RCU_SIDIS, RCU_SI_CORE(n));
	if (ret)
		dev_err(dev, "SHARC+ core %u does not enter reset\n", n);

	return ret;
}

/*
 * The SHARC+ cores run code of their own (the boot ROM's idle loop, a
 * boot stream's application or one started by U-Boot), which may access
 * the OTP. Hold them in reset; the next reset releases them again.
 */
static int adi_sc58x_hold_sharcs(struct udevice *dev)
{
	unsigned int n;
	int ret;

	for (n = SC58X_SHARC_FIRST; n <= SC58X_SHARC_LAST; n++) {
		ret = adi_sc58x_hold_core(dev, n);
		if (ret)
			return ret;
	}

	return 0;
}

/* ------------------------------------------------------------------------
 * Programming and locking
 */

/*
 * Program the write units of @req->delta in field @f with otp_pgm(),
 * which also gets the counter value @count.
 */
static int adi_sc58x_program_field(struct udevice *dev,
				   const struct adi_otp_request *req,
				   const struct adi_otp_field *f, u32 count)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	const u32 *delta = req->delta + f->offset / sizeof(u32);
	const struct adi_sc58x_field *rom_field = f->soc_data;
	u32 *member = (u32 *)((u8 *)state->otp_data + rom_field->otp_data_off);
	int ret;

	/* Null addresses and zero values are left alone by the ROM */
	memset(state->otp_data, 0, sizeof(*state->otp_data));
	state->otp_data->antiroll_nv_cntr = count;
	if (f->kind == ADI_OTP_VALUE)
		*member = delta[0];
	else
		*member = (u32)map_to_sysmem(delta);

	ret = adi_sc58x_rom_pgm(dev, f->name);

	/* A value field holds the data itself */
	memset(state->otp_data, 0, sizeof(*state->otp_data));
	return ret;
}

/*
 * Field by field, in OTP order, each read back before the next is
 * programmed (anomaly 20000129); the counter last. otp_pgm() always also
 * programs the counter, so the data fields are programmed with its current
 * value.
 */
static int adi_sc58x_otp_apply(struct udevice *dev,
			       const struct adi_otp_request *req)
{
	struct adi_sc58x_otp *state = to_sc58x(dev);
	const struct adi_otp_soc *soc = adi_otp_get_soc(dev);
	const struct adi_otp_field *counter =
		adi_otp_field_at(soc, SC58X_ANTIROLL);
	unsigned int i;
	u32 count;
	int ret;

	ret = adi_sc58x_hold_sharcs(dev);
	if (ret)
		return ret;

	if (req->counter) {
		count = req->count;
	} else {
		ret = adi_sc58x_otp_read_counter(dev, counter, &count);
		if (ret)
			return ret;
	}

	for (i = 0; i < soc->nfields; i++) {
		const struct adi_otp_field *f = &soc->fields[i];

		if (!adi_otp_request_programs(req, f->offset,
					      adi_otp_field_bytes(f)))
			continue;

		schedule();
		ret = adi_sc58x_program_field(dev, req, f, count);
		if (!ret)
			ret = adi_otp_verify_field(dev, req, f);
		if (ret) {
			dev_err(dev, "programming %s failed; it may be partly programmed\n",
				f->name);
			return ret;
		}
	}

	if (req->counter && req->new_count != req->count) {
		schedule();
		memset(state->otp_data, 0, sizeof(*state->otp_data));
		state->otp_data->antiroll_nv_cntr = req->new_count;
		ret = adi_sc58x_rom_pgm(dev, counter->name);
		if (ret)
			return ret;
	}

	return 0;
}

static int adi_sc58x_otp_lock(struct udevice *dev)
{
	int ret;

	ret = adi_sc58x_hold_sharcs(dev);
	if (ret)
		return ret;

	return adi_sc58x_rom_lock(dev);
}

/* ------------------------------------------------------------------------
 * Probe
 */

/* The CGU registers @name of the clock node */
static void __iomem *adi_sc58x_map_cgu(struct udevice *dev, ofnode clk,
				       const char *name)
{
	int index = ofnode_stringlist_search(clk, "reg-names", name);
	fdt_addr_t addr;

	if (index < 0)
		return NULL;
	addr = ofnode_get_addr_index(clk, index);
	if (addr == FDT_ADDR_T_NONE)
		return NULL;

	return map_physmem(addr, SC58X_REGS_SIZE, MAP_NOCACHE);
}

static int adi_sc58x_otp_probe(struct udevice *dev)
{
	const struct adi_otp_soc *soc = adi_otp_get_soc(dev);
	struct ofnode_phandle_args args;
	struct adi_sc58x_otp *state;
	unsigned int i, words;
	fdt_addr_t otpc, rcu;
	ofnode clk;
	int ret;

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	/* The ROM drives OTPC0; its registers are checked around the calls */
	otpc = dev_read_addr(dev);
	if (otpc == FDT_ADDR_T_NONE)
		return -EINVAL;
	state->otpc = map_physmem(otpc, SC58X_REGS_SIZE, MAP_NOCACHE);

	ret = dev_read_phandle_with_args(dev, "adi,rcu", NULL, 0, 0, &args);
	if (ret) {
		dev_err(dev, "missing adi,rcu\n");
		return ret;
	}
	rcu = ofnode_get_addr(args.node);
	if (rcu == FDT_ADDR_T_NONE)
		return -EINVAL;
	state->rcu = map_physmem(rcu, SC58X_REGS_SIZE, MAP_NOCACHE);

	clk = ofnode_by_compatible(ofnode_null(), "adi,sc58x-clocks");
	state->cgu0 = adi_sc58x_map_cgu(dev, clk, "cgu0");
	state->cgu1 = adi_sc58x_map_cgu(dev, clk, "cgu1");
	if (!state->cgu0 || !state->cgu1) {
		dev_err(dev, "no CGU registers (adi,sc58x-clocks node)\n");
		return -EINVAL;
	}

	/* rom_buf takes any one field */
	words = 0;
	for (i = 0; i < soc->nfields; i++)
		words = max_t(unsigned int, words, soc->fields[i].words);
	state->rom_buf_words = words;

	state->otp_data = devm_kzalloc(dev, sizeof(*state->otp_data),
				       GFP_KERNEL);
	state->rom_buf = devm_kcalloc(dev, words, sizeof(u32), GFP_KERNEL);
	if (!state->otp_data || !state->rom_buf)
		return -ENOMEM;
	adi_otp_set_soc_priv(dev, state);

	if (adi_sc58x_otpc_stat(state) & OTPC_STAT_ERRORS)
		dev_warn(dev, "OTPC_STAT reports an earlier error (0x%08x)\n",
			 adi_sc58x_otpc_stat(state));

	return 0;
}

static const struct adi_otp_soc_ops adi_sc58x_otp_ops = {
	.probe		= adi_sc58x_otp_probe,
	.lock_state	= adi_sc58x_otp_lock_state,
	.read		= adi_sc58x_otp_read,
	.read_counter	= adi_sc58x_otp_read_counter,
	.prepare	= adi_sc58x_otp_prepare,
	.check		= adi_sc58x_otp_check,
	.apply		= adi_sc58x_otp_apply,
	.lock		= adi_sc58x_otp_lock,
};

const struct adi_sc58x_soc adi_sc58x_soc = {
	.soc = {
		.name		= "ADSP-SC58x",
		.id		= ADI_OTP_SC58X_ID,
		.flags		= ADI_OTP_SC58X_FLAG_FSN_COPY |
				  ADI_OTP_SC58X_FLAG_FSN_IGNORE |
				  ADI_OTP_SC58X_FLAG_EMU_KEY_BLANK |
				  ADI_OTP_SC58X_FLAG_BOOT_MODE_UNCHECKED |
				  ADI_OTP_SC58X_FLAG_CGU_UNCHECKED |
				  ADI_OTP_SC58X_FLAG_DMC_UNCHECKED,
		.fields		= adi_sc58x_otp_fields,
		.nfields	= ARRAY_SIZE(adi_sc58x_otp_fields),
		.size		= SC58X_OTP_SIZE,
		/* Each 16-bit half-word has its own ECC */
		.write_size	= sizeof(u16),
		.guards		= adi_sc58x_otp_guards,
		.nguards	= ARRAY_SIZE(adi_sc58x_otp_guards),
		.ops		= &adi_sc58x_otp_ops,
	},
	.rom		= &adi_sc58x_rom_otp_api,
};
