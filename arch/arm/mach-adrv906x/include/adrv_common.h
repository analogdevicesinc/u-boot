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
#define MAX_NODE_STRING_LENGTH     200

int common_kernel_fdt_fixup(void *blob);
int arch_misc_init_common(u64 boot_addr, u64 qspi_0_base_addr);
int get_boot_slot(const char **boot_slot);
int get_te_boot_slot(const char **boot_slot);
int get_kaslr_seed(u64 *kaslr_seed);
int get_boot_device(const char **boot_device);
int get_lifecycle_state(const char **description, u32 *deployed);
int get_enforcement_counter(void);
char *get_platform(void);
void plat_log_error(char *message);
int get_dt_error_num(void);
int get_reset_cause(void);

/* To be implemented by SoC-specific layer */
bool is_boot_device_active(const char *boot_device);

#endif
