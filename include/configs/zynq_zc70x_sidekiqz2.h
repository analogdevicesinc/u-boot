/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2018 Analog Devices Inc. / Epiq Solutions
 *
 * Configuration for the Epiq Solutions SidekiqZ2 board
 */

#ifndef __CONFIG_ZYNQ_ZC70X_SIDEKIQZ2_H
#define __CONFIG_ZYNQ_ZC70X_SIDEKIQZ2_H

#define CFG_EXTRA_ENV_SETTINGS \
	"env_version=2\0" \
	"modeboot=qspiboot\0" \
	"ethaddr=00:0a:35:00:01:22\0" \
	"ipaddr=192.168.3.1\0" \
	"ipaddr_host=192.168.3.9\0" \
	"netmask=255.255.255.0\0" \
	"kernel_image=uImage\0" \
	"fit_load_address=0x2080000\0" \
	"fit_config=config@9\0" \
	"extraenv_load_address=0x207E000\0" \
	"ramdisk_image=uramdisk.image.gz\0" \
	"ramdisk_load_address=0x4000000\0" \
	"devicetree_image=devicetree.dtb\0" \
	"devicetree_load_address=0x2000000\0" \
	"dfu_alt_info=boot.dfu raw 0x0 0x100000;firmware.dfu raw 0x200000 0x1E00000;" \
		"uboot-extra-env.dfu raw 0xFF000 0x1000;uboot-env.dfu raw 0x100000 0x20000;" \
		"spare.dfu raw 0x120000 0xE0000\0" \
	"bitstream_image=system.bit.bin\0" \
	"boot_image=BOOT.bin\0" \
	"loadbit_addr=0x100000\0" \
	"loadbootenv_addr=0x2000000\0" \
	"fit_size=0x900000\0" \
	"devicetree_size=0x20000\0" \
	"ramdisk_size=0x400000\0" \
	"bitstream_size=0x400000\0" \
	"boot_size=0xF00000\0" \
	"fdt_high=0x20000000\0" \
	"initrd_high=0x20000000\0" \
	"bootenv=uEnv.txt\0" \
	"maxcpus=2\0" \
	"clear_reset_cause=mw f8000008 df0d && mw f8000258 00400000 && mw f8000004 767b\0" \
	"dfu_ram=echo Entering DFU RAM mode ... && run dfu_ram_info && dfu 0 ram 0\0" \
	"dfu_ram_info=setenv dfu_alt_info dummy.dfu ram 0 0\\\\;firmware.dfu ram ${fit_load_address} 0x1E00000\0" \
	"dfu_sf=gpio set 15;setenv silent;echo Entering DFU SF mode ... && run dfu_sf_info && " \
		"dfu 0 sf 0:0:40000000:0 && if test -n ${dfu_alt_num} && test ${dfu_alt_num} = 1; " \
		"then setenv fit_size ${filesize} && setenv dfu_alt_num && env save; fi;gpio clear 15;\0" \
	"dfu_sf_info=setenv dfu_alt_info boot.dfu raw 0x0 0x100000\\\\;firmware.dfu raw 0x200000 0x1E00000\\\\;" \
		"uboot-extra-env.dfu raw 0xFF000 0x1000\\\\;uboot-env.dfu raw 0x100000 0x20000\\\\;" \
		"spare.dfu raw 0x120000 0xE0000\0" \
	"adi_loadvals=fdt addr ${fit_load_address} && fdt get value fdt_choosen /configurations/${fit_config}/ fdt && " \
		"fdt get addr fdtaddr /images/${fdt_choosen} data && fdt addr ${fdtaddr}; " \
		"if test -n ${ad936x_ext_refclk} && test ! -n ${ad936x_skip_ext_refclk}; then " \
			"fdt set /clocks/clock@0 clock-frequency ${ad936x_ext_refclk}; " \
		"fi; " \
		"if test -n ${model}; then " \
			"fdt set / model ${model}; " \
		"fi; " \
		"if test -n ${attr_name} && test -n ${attr_val}; then " \
			"fdt set /amba/spi@e0006000/ad9361-phy@0 ${attr_name} ${attr_val} || fdt set /axi/spi@e0006000/ad9361-phy@0 ${attr_name} ${attr_val}; " \
		"fi \0" \
	"qspiboot_extraenv=sf read ${extraenv_load_address} 0xFF000 0x1000 && " \
		"env import -c ${extraenv_load_address} 0x1000 || true \0" \
	"read_sf=sf probe && run qspiboot_extraenv && " \
		"sf read ${fit_load_address} 0x200000 ${fit_size} && " \
		"iminfo ${fit_load_address} || " \
		"sf read ${fit_load_address} 0x200000 0x1E00000; \0" \
	"ramboot_verbose=adi_hwref;echo Copying Linux from DFU to RAM... && run dfu_ram;" \
		"if run adi_loadvals; then " \
		"echo Loaded AD936x refclk frequency and model into devicetree; " \
		"fi; " \
		"envversion;setenv bootargs console=ttyPS0,115200 maxcpus=${maxcpus} rootfstype=ramfs root=/dev/ram0 rw earlyprintk clk_ignore_unused uboot=\"${uboot-version}\" && " \
		"bootm ${fit_load_address}#${fit_config}\0" \
	"qspiboot_verbose=adi_hwref;echo Copying Linux from QSPI flash to RAM... && " \
		"run read_sf && " \
		"if run adi_loadvals; then " \
		"echo Loaded AD936x refclk frequency and model into devicetree; " \
		"fi; " \
		"envversion;setenv bootargs console=ttyPS0,115200 maxcpus=${maxcpus} rootfstype=ramfs root=/dev/ram0 rw earlyprintk clk_ignore_unused uboot=\"${uboot-version}\" && " \
		"bootm ${fit_load_address}#${fit_config} || echo BOOT failed entering DFU mode ... && run dfu_sf \0" \
	"qspiboot=adi_hwref; setenv silent; " \
		"if gpio input 48; then " \
			"echo DFU pin asserted && run dfu_sf; " \
		"fi; " \
		"setenv silent;" \
		"itest *f8000258 == 480003 && run clear_reset_cause && run dfu_sf; " \
		"itest *f8000258 == 480007 && run clear_reset_cause && run ramboot_verbose; " \
		"itest *f8000258 == 480006 && run clear_reset_cause && run qspiboot_verbose; " \
		"itest *f8000258 == 480002 && run clear_reset_cause && exit; " \
		"echo Booting silently && setenv silent 1; run read_sf && run adi_loadvals; " \
		"envversion;setenv bootargs console=ttyPS0,115200 maxcpus=${maxcpus} rootfstype=ramfs root=/dev/ram0 rw quiet loglevel=4 clk_ignore_unused uboot=\"${uboot-version}\" && " \
		"bootm ${fit_load_address}#${fit_config} || setenv silent;echo BOOT failed entering DFU mode ... && run dfu_sf \0" \
	"jtagboot=env default -a;sf probe && sf protect unlock 0 100000 && run dfu_sf; \0" \
	"thor_ram=run dfu_ram_info && thordown 0 ram 0\0" \
	"uenvboot=" \
		"if run loadbootenv; then " \
			"echo Loaded environment from ${bootenv}; " \
			"run importbootenv; " \
		"fi; " \
		"if test -n $uenvcmd; then " \
			"echo Running uenvcmd ...; " \
			"run uenvcmd; " \
		"fi\0" \
	"usbboot=if usb start; then " \
			"run uenvboot; " \
			"echo Copying Linux from USB to RAM... && " \
			"load usb 0 ${fit_load_address} ${kernel_image} && " \
			"load usb 0 ${devicetree_load_address} ${devicetree_image} && " \
			"load usb 0 ${ramdisk_load_address} ${ramdisk_image} && " \
			"bootm ${fit_load_address} ${ramdisk_load_address} ${devicetree_load_address}; " \
		"fi\0"

#include <configs/zynq-common.h>

#endif /* __CONFIG_ZYNQ_ZC70X_SIDEKIQZ2_H */
