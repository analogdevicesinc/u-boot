/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#ifndef _DDRINIT_H_
#define _DDRINIT_H_

#include <stdio.h>

s32 get_mails(u8 mode);
void get_streaming_message(void);
int wait_ddrphy_training_complete(void);

/*
 * Reference to Application Note section 3.4.2.1 Major Messages
 */
#define DDR_PHY_MAJMSG_TRAINING_SUCCESS 0x07  /* Training completed successfully */
#define DDR_PHY_MAJMSG_STREAMING        0x08  /* Streaming message with additional data */
#define DDR_PHY_MAJMSG_TRAINING_FAILED  0xFF  /* Training failed */
#define DDR_PHY_MAJMSG_TIMEOUT_MAX      10000000  /* Mailbox timeout iterations */

/* Mailbox operation timeout and error codes */
#define DDR_PHY_MAILBOX_TIMEOUT_US      1000000   /* 1 second timeout for mailbox operations */
#define DDR_PHY_MAILBOX_EMPTY           -1        /* No message in mailbox */
#define DDR_PHY_MAILBOX_ACK_FAILED      -2        /* ACK handshake failed */
#define DDR_PHY_MAILBOX_WAIT_TIMEOUT    -3        /* Timeout waiting for message */

#endif /* _DDRINIT_H_ */
