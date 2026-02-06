/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#ifndef __ADI_ADRV906X_COMMON_H__
#define __ADI_ADRV906X_COMMON_H__

#define BOOT_DEV_SD_0 "sd0"
#define BOOT_DEV_EMMC_0 "emmc0"
#define BOOT_DEV_QSPI_0 "qspi0"
#define BOOT_DEV_HOST "host"

#define MAX_NODE_NAME_LENGTH     80

int common_kernel_fdt_fixup(void *blob);
int arch_misc_init_common(uint64_t boot_addr, uint64_t qspi_0_base_addr);
int get_boot_slot(const char **boot_slot);
int get_te_boot_slot(const char **boot_slot);
int get_kaslr_seed(uint64_t *kaslr_seed);
int get_boot_device(const char **boot_device);
int get_lifecycle_state(const char **description, uint32_t *deployed);
int get_enforcement_counter(void);
char *get_platform(void);

/* To be implemented by SoC-specific layer */
bool is_boot_device_active(const char *boot_device);

#endif
