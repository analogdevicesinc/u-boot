/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#include <asm/global_data.h>
#include <linux/libfdt.h>
#include <init.h>
#include <asm/system.h>


#if defined(CONFIG_OF_BOARD_SETUP)
int ft_board_setup(void *blob, struct bd_info *bd)
{
	return 0;
}
#endif

int board_init(void)
{
	return 0;
}
