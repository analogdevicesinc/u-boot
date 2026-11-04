// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Analog Devices, Inc.
 */

#include <fdtdec.h>
#include <init.h>

DECLARE_GLOBAL_DATA_PTR;

int board_init(void)
{
	return 0;
}

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}
