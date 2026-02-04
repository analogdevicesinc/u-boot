/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Analog Devices, Inc.
 */

#ifndef _PLAT_STATUS_REG_H
#define _PLAT_STATUS_REG_H

/*
 * List of reasons reset was performed which gets stored in RESET_CAUSE
 * This enum MUST MATCH those defined in arm-trusted-firmware (/plat/adi/adrv/common/include/plat_status_reg.h) and linux (/drivers/soc/adi/adrv906x-err.c)
 */
enum reset_cause {
	COLD_BOOT,
	WARM_RESET,
	IMG_VERIFY_FAIL,
	WATCHDOG_RESET,
	CACHE_ECC_ERROR,
	DRAM_ECC_ERROR,
	DRAM_INIT_ERROR,
	MCS_FAIL,
	MBIAS_CAL_FAIL,
	OTHER_RESET_CAUSE
};

/* This enum must match the enumeration found in arm-trusted-firmware here: /plat/adi/adrv/common/include/plat_status_reg.h */
enum status_reg_id {
	RESET_CAUSE_NS,
	RESET_CAUSE,
	BOOT_CNT,
	STARTING_SLOT,
	LAST_SLOT
};

unsigned int rd_status_reg(enum status_reg_id reg);

bool wr_status_reg(enum status_reg_id reg, unsigned int value);

#endif
