/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration for ADI ADRV906X SoC
 * (C) Copyright 2021 Analog Devices, Inc.
 */

#ifndef __ADI_ADRV906X_EVAL_H
#define __ADI_ADRV906X_EVAL_H

#include <linux/sizes.h>

#define CONFIG_SYS_LOAD_ADDR            0x8000000

#define CONFIG_SYS_INIT_SP_ADDR         (CONFIG_SYS_TEXT_BASE)

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN           (SZ_1M)

#define CONFIG_SYS_BOOTM_LEN            (SZ_64M)

/* TODO: Remove all ADRV906X_UBOOT_STANDALONE items */
#ifdef ADRV906X_UBOOT_STANDALONE
/* Only needed for standalone (i.e. no TF-A) */
#define COUNTER_FREQUENCY               12500000        /* 400MHz HSDIG / 32 */
#define CONFIG_GICV3
#define GICD_BASE       0x21000000
#define GICR_BASE       0x21040000
#endif /* ADRV906X_UBOOT_STANDALONE */

/* ADI SDHCI driver uses SDMA (max 512KB individual DMA jobs), and a
 * block size of 512 bytes. This results in 512K / 512 = 1K max blocks.
 */
#define CONFIG_SYS_MMC_MAX_BLK_COUNT    1024

#endif /* __ADI_ADRV906X_EVAL_H */
