// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <init.h>
#include <log.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>

DECLARE_GLOBAL_DATA_PTR;

#define REG_RCU0_CRCTL		0x3108C008
#define REG_RCU0_CRSTAT		0x3108C00C
#define REG_RCU0_SVECT2		0x3108C034
#define REG_RCU0_MSG		0x3108C06C
#define REG_RCU0_MSG_CLR	0x3108C074

#define RCU_MSG_C2IDLE		BIT(10)
#define RCU_CR(n)		BIT(n)

int sc846_reset_secondary_core(void)
{
	if (!(readl(REG_RCU0_MSG) & RCU_MSG_C2IDLE))
		debug("Core 2 not in idle state\n");

	writel(RCU_CR(2), REG_RCU0_CRSTAT);
	writel(RCU_CR(2), REG_RCU0_CRCTL);

	mdelay(10);

	if (!(readl(REG_RCU0_CRSTAT) & RCU_CR(2))) {
		writel(0x0, REG_RCU0_CRCTL);
		log_warning("Core 2: reset not asserted, not waking it\n");
		return -EIO;
	}

	writel(0x0, REG_RCU0_CRCTL);
	writel(RCU_CR(2), REG_RCU0_CRSTAT);
	writel(RCU_MSG_C2IDLE, REG_RCU0_MSG_CLR);
	return 0;
}

int sc846_wake_secondary_core(unsigned long entry)
{
	/* A locked SVECT2 ignores the write; don't start the core then */
	writel(entry, REG_RCU0_SVECT2);
	if (readl(REG_RCU0_SVECT2) != entry) {
		log_warning("Core 2: SVECT2 locked, not waking it\n");
		return -EIO;
	}

	return sc846_reset_secondary_core();
}

void sc846_secondary_core_status(void)
{
	printf("CPU1: %s, entry 0x%08x%s\n",
	       readl(REG_RCU0_CRCTL) & RCU_CR(2) ? "held in reset" : "out of reset",
	       readl(REG_RCU0_SVECT2),
	       readl(REG_RCU0_MSG) & RCU_MSG_C2IDLE ? ", idle in ROM" : "");
}

int arch_early_init_r(void)
{
	/* Ignore the return value as we allow to proceed with one core */
	sc846_wake_secondary_core(gd->relocaddr);
	return 0;
}
