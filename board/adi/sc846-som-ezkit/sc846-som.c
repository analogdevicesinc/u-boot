// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <config.h>
#include <phy.h>
#include <asm/u-boot.h>
#include <asm/arch-adi/sc5xx/sc5xx.h>
#include <asm/arch-adi/sc5xx/soc.h>
#include <asm/armv8/mmu.h>
#include "../carriers/somcrr.h"

int board_init(void)
{

	if (IS_ENABLED(CONFIG_ADI_CARRIER_SOMCRR2_EZKIT)) {
		adi_somcrr_init_ethernet();
	}

	sc5xx_enable_rgmii();

	return 0;
}
