/*
 * Copyright (c) 2022, Analog Devices Incorporated, All Rights Reserved
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _PLAT_STATUS_REG_H
#define _PLAT_STATUS_REG_H

/*
 * List of reasons reset was performed which gets stored in RESET_CAUSE
 * This enum MUST MATCH those defined in arm-trusted-firmware (/plat/adi/adrv/common/include/plat_status_reg.h) and linux (/drivers/soc/adi/adrv906x-err.c)
 */
typedef enum {
	RESET_VALUE,
	IMG_VERIFY_FAIL,
	WATCHDOG_RESET,
	ECC_ERROR,
	OTHER_RESET_CAUSE
} reset_cause_t;

/* This enum must match the enumeration found in arm-trusted-firmware here: /plat/adi/adrv/common/include/plat_status_reg.h */
typedef enum {
	RESET_CAUSE_NS,
	RESET_CAUSE,
	BOOT_CNT,
	STARTING_SLOT,
	LAST_SLOT
} status_reg_id_t;

unsigned int rd_status_reg(status_reg_id_t reg);

bool wr_status_reg(status_reg_id_t reg, unsigned int value);

#endif
