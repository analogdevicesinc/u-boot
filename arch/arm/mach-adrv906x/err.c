/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2021 Analog Devices, Inc.
 */

#include <env.h>
#include <errno.h>
#include <stdbool.h>
#include <sysreset.h>

#include <linux/psci.h>

#include <plat_status_reg.h>

#include <stdio.h>

void plat_error_handler(void)
{
	char *reset_cause;

	reset_cause = env_get("reset_cause");

	if (reset_cause) {
		if (!strcmp(reset_cause, "IMG_VERIFY_FAIL"))
			wr_status_reg(RESET_CAUSE_NS, IMG_VERIFY_FAIL);
		else
			wr_status_reg(RESET_CAUSE_NS, OTHER_RESET_CAUSE);
	} else {
		wr_status_reg(RESET_CAUSE_NS, OTHER_RESET_CAUSE);
	}

	psci_sys_reset(SYSRESET_WARM);

	while (1)
		;
}
