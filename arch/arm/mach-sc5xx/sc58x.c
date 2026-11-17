// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2024 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 */

#include <asm/io.h>
#include <adi_rom.h>
#include <asm/arch/sc5xx.h>
#include <asm/arch/spl.h>

#define REG_SPU0_SECUREC0 0x3108C980
#define REG_PADS0_PCFG0 0x31004404
#define REG_SPU0_SECUREP_START 0x3108CA00
#define REG_SPU0_SECUREP_END 0x3108CCF0

adi_rom_boot_fn adi_rom_boot = (adi_rom_boot_fn)0x000000e1;

/* Jump table entries (defSC58x_rom_jumptable.h); odd: Thumb code */
const struct adi_rom_otp_api adi_sc58x_rom_otp_api = {
	.get		= (void *)0x00000151,	/* FUNC_ROM_OTP_GET */
	.get_data	= (void *)0x00000149,	/* FUNC_ROM_OTP_GET_DATA */
	.pgm		= (void *)0x00000139,	/* FUNC_ROM_OTPPGM */
	.lock		= (void *)0x00000141,	/* FUNC_ROM_OTP_LOCK */
	.cfg		= (void *)0x00000169,	/* FUNC_ROM_OTP_CFG */
};

void sc5xx_enable_rgmii(void)
{
	writel((readl(REG_PADS0_PCFG0) | 0xc), REG_PADS0_PCFG0);
}

void sc5xx_soc_init(void)
{
	sc5xx_enable_ns_sharc_access(REG_SPU0_SECUREC0);
	sc5xx_disable_spu0(REG_SPU0_SECUREP_START, REG_SPU0_SECUREP_END);
	sc5xx_enable_pmu();
}
