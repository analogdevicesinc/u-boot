/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#ifndef __CONFIG_SC846_SOM_H
#define __CONFIG_SC846_SOM_H

/* Memory Settings */
#define CFG_SYS_SDRAM_BASE	0x80000000
#define CFG_SYS_SDRAM_SIZE	0x40000000

/* GIC */
#define GICD_BASE 0x31200000
#define GICR_BASE 0x31240000

#endif
