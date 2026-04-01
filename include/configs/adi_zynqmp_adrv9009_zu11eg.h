/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2019-2026 Analog Devices Inc.
 *
 * Configuration for the ADI ADRV9009-ZU11EG board
 */

#ifndef __CONFIG_ADI_ZYNQMP_ADRV9009_ZU11EG_H
#define __CONFIG_ADI_ZYNQMP_ADRV9009_ZU11EG_H

/* Initial environment variables */
#define CFG_EXTRA_ENV_SETTINGS \
	"kernel_addr=0x80000\0" \
	"initrd_addr=0xa00000\0" \
	"initrd_size=0x2000000\0" \
	"fdt_addr=4000000\0" \
	"fdt_high=0x10000000\0" \
	"loadbootenv_addr=0x100000\0" \
	"sdbootdev=0\0" \
	"kernel_offset=0x280000\0" \
	"fdt_offset=0x200000\0" \
	"kernel_size=0x1e00000\0" \
	"fdt_size=0x80000\0" \
	"bootenv=uEnv.txt\0" \
	"loadbootenv=load mmc $sdbootdev:$partid ${loadbootenv_addr} ${bootenv}\0" \
	"importbootenv=echo Importing environment from SD ...; " \
		"env import -t ${loadbootenv_addr} $filesize\0" \
	"sd_uEnvtxt_existence_test=test -e mmc $sdbootdev:$partid /uEnv.txt\0" \
	"netboot=tftpboot 10000000 image.ub && bootm\0" \
	"qspiboot=sf probe 0 0 0 && sf read $fdt_addr $fdt_offset $fdt_size && " \
		  "sf read $kernel_addr $kernel_offset $kernel_size && " \
		  "booti $kernel_addr - $fdt_addr\0" \
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
	"sdboot=mmc dev $sdbootdev && mmcinfo && run uenvboot; " \
		"load mmc $sdbootdev:$partid $fdt_addr system.dtb && " \
		"load mmc $sdbootdev:$partid $kernel_addr Image && " \
		"booti $kernel_addr - $fdt_addr\0" \
	"jtagboot=tftpboot 80000 Image && tftpboot $fdt_addr system.dtb && " \
		 "tftpboot 6000000 rootfs.cpio.ub && booti 80000 6000000 $fdt_addr\0" \
	"usbhostboot=usb start && load usb 0 $fdt_addr system.dtb && " \
		     "load usb 0 $kernel_addr Image && " \
		     "booti $kernel_addr - $fdt_addr\0"

#include <configs/xilinx_zynqmp.h>

#endif /* __CONFIG_ADI_ZYNQMP_ADRV9009_ZU11EG_H */
