/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#include <asm/io.h>
#include <log.h>

#include <adrv906x_def.h>
#include <adrv906x_status_reg.h>
#include <err.h>
#include <plat_status_reg.h>

/* This value must match the value found in arm-trusted-firmware (plat/adi/adrv/adrv906x/adrv906x_status_reg.c) and in linux (/drivers/soc/adi/adrv906x-err.c) */
#define RESET_CAUSE_NS_OFFSET              0

unsigned int rd_status_reg(status_reg_id_t reg)
{
	switch (reg) {
	case RESET_CAUSE_NS:
		return readl((void *)(A55_SYS_CFG + SCRATCH_NS + RESET_CAUSE_NS_OFFSET));

	default:
		plat_error_message("Not a valid status register");
		return 0;
	}
}

bool wr_status_reg(status_reg_id_t reg, unsigned int value)
{
	switch (reg) {
	case RESET_CAUSE_NS:
		writel(value, (void *)(A55_SYS_CFG + SCRATCH_NS + RESET_CAUSE_NS_OFFSET));
		break;

	default:
		plat_error_message("Not a valid status register");
		return false;
	}
	return true;
}
