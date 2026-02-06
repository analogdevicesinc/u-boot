/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#ifndef __ADI_ADRV906X_FDT_H__
#define __ADI_ADRV906X_FDT_H__

int get_dual_tile(uint32_t *dual_tile);
int get_secondary_linux_enabled(uint32_t *secondary_linux_enabled);
int get_sysclk_freq(void);
bool is_sysc(void);

#endif /* __ADI_ADRV906X_FDT_H__ */
