/*
 * Copyright (c) 2022, Analog Devices Incorporated, All Rights Reserved
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _ERR_H
#define _ERR_H

/* Handle errors by calling function to set reset cause and performing a psci reset */
void plat_error_handler(void);

#endif
