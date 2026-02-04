/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration for ADI ADRV906X SoC
 * (C) Copyright 2021 Analog Devices, Inc.
 */

#ifndef __ADI_ADRV906X_EVAL_H
#define __ADI_ADRV906X_EVAL_H

/* TODO: Remove all ADRV906X_UBOOT_STANDALONE items */
#ifdef ADRV906X_UBOOT_STANDALONE
/* Only needed for standalone (i.e. no TF-A) */
#define COUNTER_FREQUENCY               12500000        /* 400MHz HSDIG / 32 */
#define GICD_BASE       0x21000000
#define GICR_BASE       0x21040000
#endif /* ADRV906X_UBOOT_STANDALONE */

#endif /* __ADI_ADRV906X_EVAL_H */
