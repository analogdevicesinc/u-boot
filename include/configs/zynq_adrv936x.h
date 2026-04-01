/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2015-2026 Analog Devices Inc.
 *
 * Configuration for the Analog Devices ADRV9361/ADRV9364 boards
 */

#ifndef __CONFIG_ZYNQ_ADRV936X_H
#define __CONFIG_ZYNQ_ADRV936X_H

#define DFU_ALT_INFO_RAM \
	"dfu_ram_info=" \
	"setenv dfu_alt_info " \
	"dummy.dfu ram 0 0\\\\;" \
	"firmware.dfu ram ${fit_load_address} 0x1E00000\0" \
	"dfu_ram=echo Entering DFU RAM mode ... && run dfu_ram_info && dfu 0 ram 0\0" \
	"thor_ram=run dfu_ram_info && thordown 0 ram 0\0"

#define DFU_ALT_INFO_SF \
	"dfu_sf_info=" \
	"setenv dfu_alt_info " \
	"boot.dfu raw 0x0 0xE0000\\\\;" \
	"firmware.dfu raw 0x100000 0x500000\\\\;" \
	"uboot-env.dfu raw 0xE0000 0x20000\0" \
	"dfu_sf=echo Entering DFU SF mode ... && run dfu_sf_info && dfu 0 sf 0:0:50000000:0\0"

#define CFG_EXTRA_ENV_SETTINGS \
	"modeboot=sdboot\0" \
	"kernel_image=uImage\0" \
	"fit_load_address=0x2080000\0" \
	"fit_config=config@0\0" \
	"ramdisk_image=uramdisk.image.gz\0" \
	"ramdisk_load_address=0x4000000\0" \
	"devicetree_image=devicetree.dtb\0" \
	"devicetree_load_address=0x2000000\0" \
	"bitstream_image=system.bit.bin\0" \
	"boot_image=BOOT.bin\0" \
	"loadbit_addr=0x100000\0" \
	"loadbootenv_addr=0x2000000\0" \
	"fit_size=0x500000\0" \
	"devicetree_size=0x20000\0" \
	"ramdisk_size=0x400000\0" \
	"bitstream_size=0x400000\0" \
	"boot_size=0xF00000\0" \
	"fdt_high=0x20000000\0" \
	"initrd_high=0x20000000\0" \
	"bootenv=uEnv.txt\0" \
	"loadbootenv=load mmc 0 ${loadbootenv_addr} ${bootenv}\0" \
	"importbootenv=echo Importing environment from SD ...; " \
		"env import -t ${loadbootenv_addr} $filesize\0" \
	"sd_uEnvtxt_existence_test=test -e mmc 0 /uEnv.txt\0" \
	"uenvboot=" \
		"if run sd_uEnvtxt_existence_test; then " \
			"run loadbootenv; " \
			"echo Loaded environment from ${bootenv}; " \
			"run importbootenv; " \
		"fi; " \
		"if test -n $uenvcmd; then " \
			"echo Running uenvcmd ...; " \
			"run uenvcmd; " \
		"fi\0" \
	"sdboot=echo Booting from SD ... && " \
		"run uenvboot; " \
		"load mmc 0 ${fit_load_address} ${kernel_image} && " \
		"load mmc 0 ${devicetree_load_address} ${devicetree_image} && " \
		"load mmc 0 ${ramdisk_load_address} ${ramdisk_image} && " \
		"bootm ${fit_load_address} ${ramdisk_load_address} ${devicetree_load_address}\0" \
	"qspiboot=echo Copying Linux from QSPI flash to RAM... && " \
		"sf probe 0 && " \
		"sf read ${fit_load_address} 0x100000 ${fit_size} && " \
		"bootm ${fit_load_address}\0" \
	"usbboot=if usb start; then " \
			"run uenvboot; " \
			"echo Copying Linux from USB to RAM... && " \
			"load usb 0 ${fit_load_address} ${kernel_image} && " \
			"load usb 0 ${devicetree_load_address} ${devicetree_image} && " \
			"load usb 0 ${ramdisk_load_address} ${ramdisk_image} && " \
			"bootm ${fit_load_address} ${ramdisk_load_address} ${devicetree_load_address}; " \
		"fi\0" \
	DFU_ALT_INFO_RAM \
	DFU_ALT_INFO_SF

#include <configs/zynq-common.h>

#endif /* __CONFIG_ZYNQ_ADRV936X_H */
