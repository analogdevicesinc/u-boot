// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include "ddrinit.h"
#include "ADSP-SC84xW.h"
#include "./EHP2_LP4_1D_2000/ddr_messages.h"
#include "./EHP2_LP4_1D_2000/EHP2_LPDDR4_1D_2000_Core1.h"
#include <linux/types.h>

s32 get_mails(u8 mode)
{
	s32 mail, mail_0, mail_1;
	u32 timeout_wait = 0;
	u32 timeout_ack = 0;

	if (mode == 1)
		REG_WRITE(REG_LPDDRMISC_DBG_CTL0, 0xFFFFFFFF);

	while (((REG_READ(REG_LPDDR4_PHY_APB_UCTSHADOWREGS)) & 0x01) != 0x0) {
		asm("NOP");
		timeout_wait++;
		if (timeout_wait > DDR_PHY_MAILBOX_TIMEOUT_US) {
			printf("DDR_MAILBOX: ERROR - Timeout waiting for message! UCTSHADOWREGS stuck at 0x%x\n",
			       REG_READ(REG_LPDDR4_PHY_APB_UCTSHADOWREGS));
			return DDR_PHY_MAILBOX_WAIT_TIMEOUT;
		}
	}

	mail_0 = REG_READ(REG_LPDDR4_PHY_APB_UCTWRITEONLYSHADOW);

	if (mode == 1) {
		mail_1 = REG_READ(REG_LPDDR4_PHY_APB_UCTDATWRITEONLYSHADOW);
		mail   = (mail_1 << 16) | mail_0;
		REG_WRITE(REG_LPDDRMISC_DBG_CTL0, mail);
	} else {
		mail = mail_0;
		REG_WRITE(REG_LPDDRMISC_DBG_CTL0, (mail | 0xFFFF0000));
	}

	REG_WRITE(REG_LPDDR4_PHY_APB_DCTWRITEPROT, 0x0);

	while (((REG_READ(REG_LPDDR4_PHY_APB_UCTSHADOWREGS)) & 0x01) != 0x1) {
		asm("NOP");
		timeout_ack++;
		if (timeout_ack > DDR_PHY_MAILBOX_TIMEOUT_US) {
			printf("DDR_MAILBOX: ERROR - ACK handshake timeout! UCTSHADOWREGS=0x%x\n",
			       REG_READ(REG_LPDDR4_PHY_APB_UCTSHADOWREGS));
			/* Try to restore write protection */
			REG_WRITE(REG_LPDDR4_PHY_APB_DCTWRITEPROT, 0x1);
			return DDR_PHY_MAILBOX_ACK_FAILED;
		}
	}

	/* Complete ACK */
	REG_WRITE(REG_LPDDR4_PHY_APB_DCTWRITEPROT, 0x1);

	return mail;
}

void get_streaming_message(void)
{
	s32 string_index, arg;
	int i = 1, j;
	s32 num_args;
	u32 msg_id;
	s32 args[10];
	const char *msg_format = NULL;

	/* Get the string index and argument count */
	string_index = get_mails(1);

	/* Check for mailbox errors */
	if (string_index < 0) {
		printf("DDR: ERROR - Failed to get streaming message index ");
		printf("(error code: %d)\n", string_index);
		return;
	}

	msg_id = (string_index >> 16) & 0xFFFF;
	num_args = string_index & 0xFFFF;

	/* Read all streaming message arguments */
	for (i = 0; i < num_args && i < 16; i++) {
		arg = get_mails(1);

		/* Check for mailbox errors during streaming */
		if (arg < 0) {
			printf("DDR: ERROR - Failed to get streaming arg %d ",
			       i);
			printf("(error code: %d)\n", arg);
			return;
		}
		args[i] = arg;
	}

	/* Look up the message format string - table IDs include both message index and arg count */
	for (j = 0; j < ONED_TRAINING_MESSAGE_STRING_COUNT; j++) {
		if (ddr_1d_log_messages[j].id == (uint32_t)string_index) {
			msg_format = ddr_1d_log_messages[j].message;
			break;
		}
	}

	/* Print the decoded message */
	if (msg_format) {
		printf("DDR_FW: ");
		printf(msg_format, args[0], args[1], args[2], args[3], args[4], args[5],
		       args[6], args[7], args[8], args[9], args[10]);
	} else {
		printf("DDR_FW: Unknown message ID 0x%04x with %d args\n", msg_id, num_args);
	}
}

int wait_ddrphy_training_complete(void)
{
	s32 maj_msg;
	s32 timeout = 0;

	printf("DDR: Waiting for PHY training complete...\n");

	do {
		maj_msg = get_mails(0);

		/* Check for mailbox errors first */
		if (maj_msg == DDR_PHY_MAILBOX_WAIT_TIMEOUT) {
			printf("DDR: ERROR - Mailbox wait timeout!\n");
			return -1;
		} else if (maj_msg == DDR_PHY_MAILBOX_ACK_FAILED) {
			printf("DDR: ERROR - Mailbox ACK handshake failed!\n");
			return -1;
		} else if (maj_msg == DDR_PHY_MAILBOX_EMPTY) {
			printf("DDR: WARNING - Mailbox empty (unexpected)\n");
			/* Continue waiting */
		}
		/* Handle valid messages */
		else if (maj_msg == DDR_PHY_MAJMSG_STREAMING) {
			printf("DDR: FW streaming message (0x%02x)\n",
			       DDR_PHY_MAJMSG_STREAMING);
			get_streaming_message();
		} else if (maj_msg != DDR_PHY_MAJMSG_TRAINING_SUCCESS &&
			   maj_msg != DDR_PHY_MAJMSG_TRAINING_FAILED) {
			printf("DDR: FW message 0x%02x\n", maj_msg);
		}

		timeout++;
		if (timeout > DDR_PHY_MAJMSG_TIMEOUT_MAX) {
			printf("DDR: ERROR - Firmware timeout! Last message: 0x%02x\n",
			       maj_msg);
			return -1;
		}
	} while ((maj_msg != DDR_PHY_MAJMSG_TRAINING_SUCCESS) &&
		 (maj_msg != DDR_PHY_MAJMSG_TRAINING_FAILED));

	if (maj_msg == DDR_PHY_MAJMSG_TRAINING_SUCCESS) {
		printf("DDR: PHY training SUCCESS (0x%02x)\n",
		       DDR_PHY_MAJMSG_TRAINING_SUCCESS);
		return 0;
	}

	printf("DDR: PHY training FAILED (0x%02x)\n", maj_msg);
	return -1;
}
