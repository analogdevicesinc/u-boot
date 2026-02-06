/*
 * Copyright (c) 2022, Analog Devices Inc.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef TA_EXAMPLE_EARLY_H
#define TA_EXAMPLE_EARLY_H


/*
 * This UUID must match the one defined in
 * the trusted application's header.
 */
#define TA_EXAMPLE_EARLY_UUID \
	{ \
		0x9d05995e, \
		0x0c48, 0x4d8f, \
		{ \
			0xad, 0x52, \
			0x29, 0x04, \
			0x9d, 0x9f, \
			0xd2, 0x77, \
		} \
	}

/* The function IDs implemented in this TA.
 * Note: This must be kept in sync with the
 * enum in the trusted application's header.
 */
enum ta_example_early_cmds {
	TA_EXAMPLE_EARLY_CMD_DUMMY,
	/* New commands go above this comment.
	 * Keep 'COUNT' as the last entry. */
	TA_EXAMPLE_EARLY_CMDS_COUNT
};

#endif /* TA_EXAMPLE_EARLY_H */
