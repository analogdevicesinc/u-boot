// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <command.h>
#include <cpu_func.h>
#include <stdio.h>
#include <vsprintf.h>
#include <asm/arch/sc5xx.h>
#include <asm/types.h>

/* BOOTROM idle loop address */
#define SC846_ROM_IDLE_LOOP	0x288

int is_core_valid(unsigned int core)
{
	return core < 2;
}

int cpu_status(u32 nr)
{
	if (nr == 0) {
		printf("CPU0: boot CPU, running U-Boot\n");
		return 0;
	}

	sc846_secondary_core_status();
	return CMD_RET_SUCCESS;
}

int cpu_reset(u32 nr)
{
	if (nr != 1) {
		printf("CPU0: boot CPU, running U-Boot\n");
		return CMD_RET_USAGE;
	}

	if (sc846_reset_secondary_core())
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

int cpu_disable(u32 nr)
{
	if (nr != 1) {
		printf("CPU0: boot CPU, running U-Boot\n");
		return CMD_RET_USAGE;
	}

	/*
	 * Holding one core in reset while the other is running just did not work
	 * out. Hence, we just disable the secondary core by sending it to the
	 * BOOTROM idle loop.
	 */
	if (sc846_wake_secondary_core(SC846_ROM_IDLE_LOOP))
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

int cpu_release(u32 nr, int argc, char *const argv[])
{
	unsigned int addr;

	if (argc != 1)
		return CMD_RET_USAGE;
	if (nr != 1) {
		printf("CPU0: boot CPU, running U-Boot\n");
		return CMD_RET_USAGE;
	}

	addr = hextoul(argv[0], NULL);
	/* The image may still be in our D-cache, CPU1 reads DDR directly */
	flush_dcache_all();
	if (sc846_wake_secondary_core(addr))
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}
