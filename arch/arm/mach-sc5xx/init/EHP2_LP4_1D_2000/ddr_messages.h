/*
 * ddr_messages.h
 *
 *  Created on: Nov 3, 2025
 *      Author: MChitne2
 */

/*
 * Copyright(c) 2024, Analog Devices Incorporated - All Rights Reserved
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef DDR_MESSAGES_H_
#define DDR_MESSAGES_H_

#include <stdint.h>
#include <stdlib.h>
typedef struct {
	uint32_t id;
	char *message;
} ddr_message_t;

#define ONED_TRAINING_MESSAGE_STRING_COUNT 412
#define TWOD_TRAINING_MESSAGE_STRING_COUNT 569
extern const ddr_message_t ddr_1d_log_messages[ONED_TRAINING_MESSAGE_STRING_COUNT];
extern const ddr_message_t ddr_2d_log_messages[TWOD_TRAINING_MESSAGE_STRING_COUNT];

#endif /* DDR_MESSAGES_H_ */



