/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#include <env.h>
#include <errno.h>
#include <log.h>
#include <stdbool.h>
#include <sysreset.h>
#include <vsprintf.h>

#include <linux/psci.h>

#include <adrv_common.h>
#include <plat_status_reg.h>

#define DT_LOG_MESSAGE_MAX              768

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

/* Log message in device tree */
void plat_log_dt_message(char *label, char *message)
{
	char log[MAX_NODE_STRING_LENGTH + 9];

	if (get_dt_error_num() >= DT_LOG_MESSAGE_MAX) {
		log_info("Unable to log message to device tree, maximum exceeded\n");
		return;
	}

	/* Add label to beginning of message */
	memcpy(log, label, strlen(label));
	memcpy(log + strlen(label), message, strlen(message) + 1);

	/* Log to device tree */
	plat_log_error(log);
}

/* Record warning message in device tree and to UART */
void plat_warn_message(char *fmt, ...)
{
	char message[MAX_NODE_STRING_LENGTH];
	va_list args;

	va_start(args, fmt);
	vsnprintf(message, MAX_NODE_STRING_LENGTH, fmt, args);
	va_end(args);

	plat_log_dt_message("warning: ", message);
	log_warning("%s\n", message);
}

/* Record error message in device tree and to UART */
void plat_error_message(char *fmt, ...)
{
	char message[MAX_NODE_STRING_LENGTH];
	va_list args;

	va_start(args, fmt);
	vsnprintf(message, MAX_NODE_STRING_LENGTH, fmt, args);
	va_end(args);

	plat_log_dt_message("err: ", message);
	log_err("%s\n", message);
}
