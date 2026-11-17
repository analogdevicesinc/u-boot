// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Making the 32-bit ADSP-SC5xx boot ROM executable for calls from U-Boot
 * proper.
 *
 * U-Boot runs with a 1:1 translation table: DRAM is cacheable and
 * everything else, the boot ROM at 0x0 included, is mapped as
 * DCACHE_OFF, i.e. strongly-ordered and execute-never (XN). The ROM API
 * takes physical addresses, which the 1:1 map leaves unchanged, so the
 * only change needed is to clear XN on the ROM's 1 MiB section while it
 * is used. The memory type stays strongly-ordered, which is what the ROM
 * sees with the MMU off.
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <adi_rom.h>
#include <asm/system.h>
#include <linux/sizes.h>

/* The boot ROM occupies the first 1 MiB section of the ARM address map */
#define ADI_ROM_BASE	0x00000000
#define ADI_ROM_SIZE	SZ_1M

static unsigned int adi_rom_users;

static void adi_rom_set_executable(bool executable)
{
	enum dcache_option option = DCACHE_OFF;

	if (!(get_cr() & CR_M))
		return;		/* MMU off: nothing is execute-never */

	if (executable)
		option = (enum dcache_option)(DCACHE_OFF & ~TTB_SECT_XN_MASK);
	mmu_set_region_dcache_behaviour(ADI_ROM_BASE, ADI_ROM_SIZE, option);
}

void adi_rom_enter(void)
{
	if (!adi_rom_users++)
		adi_rom_set_executable(true);
}

void adi_rom_exit(void)
{
	if (adi_rom_users && !--adi_rom_users)
		adi_rom_set_executable(false);
}
