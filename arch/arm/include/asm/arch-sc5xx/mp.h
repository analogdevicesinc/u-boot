// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#ifndef ARCH_ADI_MP_H
#define ARCH_ADI_MP_H

#ifndef __ASSEMBLY__

#include <linux/types.h>
extern void *secondary_boot_addr;
extern void *secondary_boot_code_start;
extern u32 secondary_boot_func;
extern u64 secondary_boot_code_size;

void *get_spin_tbl_addr(void);
#endif

#endif /* ARCH_ADI_MP_H */