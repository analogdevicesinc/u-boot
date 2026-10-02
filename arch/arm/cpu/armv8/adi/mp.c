// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <config.h>
#include <asm/io.h>
#include <asm/arch/mp.h>
#include <cpu_func.h>
#include <log.h>
#include <linux/types.h>
#include <linux/delay.h>

#define REG_RCU0_CTL          0x3108C000 /*  RCU0 Control Register */
#define REG_RCU0_STAT         0x3108C004 /*  RCU0 Status Register */
#define REG_RCU0_CRCTL        0x3108C008 /*  RCU0 Core Reset Outputs Control Register */
#define REG_RCU0_CRSTAT       0x3108C00C /*  RCU0 Core Reset Outputs Status Register */
#define REG_RCU0_SRRQSTAT     0x3108C018 /*  RCU0 System Reset Request Status Register */
#define REG_RCU0_SIDIS        0x3108C01C /*  RCU0 System Interface Disable Register */
#define REG_RCU0_SISTAT       0x3108C020 /*  RCU0 System Interface Status Register */
#define REG_RCU0_SVECT_LCK    0x3108C024 /*  RCU0 SVECT Lock Register */
#define REG_RCU0_BCODE        0x3108C028 /*  RCU0 Boot Code Register */
#define REG_RCU0_SVECT0       0x3108C02C /*  RCU0 Software Vector Register 0 */
#define REG_RCU0_SVECT1       0x3108C030 /*  RCU0 Software Vector Register 1 */
#define REG_RCU0_SVECT2       0x3108C034 /*  RCU0 Software Vector Register 2 */
#define REG_RCU0_MSG          0x3108C06C /*  RCU0 Message Register */
#define REG_RCU0_MSG_SET      0x3108C070 /*  RCU0 Message Set Bits Register */
#define REG_RCU0_MSG_CLR      0x3108C074 /*  RCU0 Message Clear Bits Register */

#define RCU_MSG_CALLBACK     30 /*  Callback Call Flag */
#define RCU_MSG_CALLINIT     29 /*  Call Initcode Flag */
#define RCU_MSG_CALLAPP      28 /*  Call Application Flag */
#define RCU_MSG_HALTONCALL   26 /*  Halt on Callback Call */
#define RCU_MSG_HALTONINIT   25 /*  Halt on Initcode Call */
#define RCU_MSG_HALTONAPP    24 /*  Halt on Application Call */
#define RCU_MSG_L3INIT       23 /*  L3 Initialized */
#define RCU_MSG_L2INIT       22 /*  L2 Initialized */
#define RCU_MSG_C2ACTIVATE   20 /*  Core 2 Activated */
#define RCU_MSG_C1ACTIVATE   19 /*  Core 1 Activated */
#define RCU_MSG_C2L1INIT     18 /*  Core 2 L1 Initialized */
#define RCU_MSG_C1L1INIT     17 /*  Core 1 L1 Initialized */
#define RCU_MSG_C0L1INIT     16 /*  Core 0 L1 Initialized */
#define RCU_MSG_C2IDLE       10 /*  Core 2 Idle */
#define RCU_MSG_C1IDLE        9 /*  Core 1 Idle */
#define RCU_MSG_C0IDLE        8 /*  Core 0 Idle */
#define RCU_MSG_ERRCODE       0 /*  ROM Error Code */

#define BITP_RCU_CRSTAT_CR0 0 /*  Core Reset Outputs */
#define BITP_RCU_CRSTAT_CR1 1 /*  Core Reset Outputs */
#define BITP_RCU_CRSTAT_CR2 2 /*  Core Reset Outputs */
#define BITP_RCU_CRSTAT_CR3 3 /*  Core Reset Outputs */

static void wake_secondary_core(u32 coreid)
{
	u32 ret;
	u32 slock;
	u32 rcu_msg;

	if (coreid != 2) {
		debug("only Core2 supported\n");
		return;
	}

	rcu_msg = readl(REG_RCU0_MSG);
	if (!(rcu_msg & RCU_MSG_C2IDLE)) {
		debug("Core 2 not in idle state\n");
	}

	slock = readl(REG_RCU0_SVECT_LCK);
	if (slock) {
		debug("SVECT's for cores are locked\n");
	}
	writel((secondary_boot_func), REG_RCU0_SVECT2);

	writel(1 << coreid, REG_RCU0_CRSTAT);
	writel(1 << coreid, REG_RCU0_CRCTL);

	udelay(10000);

	ret = readl(REG_RCU0_CRSTAT);
	if (!(ret & (1 << BITP_RCU_CRSTAT_CR2)))
		printf("Core2 reset not asserted\n");
	writel(0x0, REG_RCU0_CRCTL);

	writel(1 << coreid, REG_RCU0_CRSTAT);
	writel(1 << RCU_MSG_C2IDLE, REG_RCU0_MSG_CLR);
	writel(1 << RCU_MSG_C2ACTIVATE, REG_RCU0_MSG_SET);
}

void *get_spin_tbl_addr(void)
{
	/* the spin table is at the beginning */
	return secondary_boot_code_start;
}

u32 cpu_mask(void)
{
	return 0x4;
}

int is_core_valid(unsigned int core)
{
	return !!((1 << core) & cpu_mask());
}

int cpu_reset(u32 nr)
{
	puts("Feature is not implemented.\n");

	return 0;
}

int cpu_disable(u32 nr)
{
	puts("Feature is not implemented.\n");

	return 0;
}

int cpu_status(u32 nr)
{
	int i;
	u64 *table = get_spin_tbl_addr();
	u32 mask = cpu_mask();

	if (mask && (1 << nr)) {
		printf("Release address for core %d located at 0x%p\n", nr, table);
	} else {
		printf("Core not supported. Only Core2 is supported");
	}
	return 0;
}

int cpu_release(u32 nr, int argc, char *const argv[])
{
	if (nr != 2) {
		puts("Only Core 2 is supported\n");
		return 0;
	}

	wake_secondary_core(nr);

	return 0;
}
