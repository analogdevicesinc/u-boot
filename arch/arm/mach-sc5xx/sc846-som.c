// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2024 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 */

#include <asm/io.h>
#include <asm/armv8/mmu.h>
#include <asm/arch/sc5xx.h>
#include <asm/arch/spl.h>

#define REG_PADS0_PCFG0 0x31004604
#define REG_RCU0_BCODE 0x3108C028

#define REG_SPU0_SECUREP_START 0x3108BA00
#define REG_SPU0_WP_START 0x3108B400
#define REG_SPU0_SECUREC0 0x3108B980

#define REG_CSTSGENWR0_CNTCR 0x3110E000  /* or 0x31149000 */

static struct mm_region sc846_mem_map[] = {
	{
		/* Peripherals */
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* DDR */
		.virt = 0x80000000UL,
		.phys = 0x80000000UL,
		.size = 0x40000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
	/* List terminator */
		0,
	}
};

struct mm_region *mem_map = sc846_mem_map;

adi_rom_boot_fn adi_rom_boot = (adi_rom_boot_fn)0x000000e4;

void sc5xx_enable_rgmii(void)
{
	// Select RGMII interface (bit 3) and deassert RGMII reset (bit 2)
	writel((readl(REG_PADS0_PCFG0) | 0xc), REG_PADS0_PCFG0);

	// Set little-endian DMA transfer format (clear bit 19)
	writel(readl(REG_PADS0_PCFG0) & ~(1 << 19), REG_PADS0_PCFG0);
}

/**
 * SPU/SMPU configuration is the default for permissive access from non-secure
 * EL1. If TFA and OPTEE are configured, they run *after* this code, as the
 * current boot flow is SPL -> TFA -> OPTEE -> Proper -> Linux, and will
 * be expected to configure peripheral security correctly. If they are not
 * configured, then this permissive setting will allow Linux (which always
 * runs in NS EL1) to control all access to these peripherals. Without it,
 * the peripherals would simply be unavailable in a non-security build,
 * which is not OK.
 */
void sc5xx_soc_init(void)
{
	phys_addr_t smpus[] = {
		0x31083800, //SMPU2
		0x31084800, //SMPU3
		0x31085800, //SMPU4
		0x31086800, //SMPU5
		0x31087800, //SMPU6
		0x310A0800, //SMPU9
		0x310A1800, //SMPU11
		0x31012800, //SMPU12
	};
	size_t i;

	// Enable coresight timer
	writel(1, REG_CSTSGENWR0_CNTCR);

	// Disable SPU and SPU WP registers
	sc5xx_disable_spu0(REG_SPU0_SECUREP_START, REG_SPU0_SECUREP_START + 4 * 213);
	sc5xx_disable_spu0(REG_SPU0_WP_START, REG_SPU0_WP_START + 4 * 213);

	/* configure smpus permissively */
	for (i = 0; i < ARRAY_SIZE(smpus); ++i)
		writel(0x500, smpus[i]);

	asm("mrs x8, scr_el3");
	asm("orr x8, x8, #(1 << 1)");
	asm("orr x8, x8, #(1 << 2)");
	asm("msr scr_el3, x8");
	isb();
	isb();
}
