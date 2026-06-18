// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 */

#include <spl.h>
#include <asm/arch/sc5xx.h>
#include <asm/arch/spl.h>
#include <linux/libfdt.h>
#include "init/clkinit.h"
#include "init/dmcinit.h"

static bool adi_start_uboot_proper;

static int adi_sf_default_bus = CONFIG_SF_DEFAULT_BUS;
static int adi_sf_default_cs = CONFIG_SF_DEFAULT_CS;
static int adi_sf_default_speed = CONFIG_SF_DEFAULT_SPEED;

u32 bmode;

int spl_start_uboot(void)
{
	sc5xx_get_boot_mode(&bmode);

	if (bmode == 0)
		return 1;

	return 0;
}

unsigned int spl_spi_get_default_speed(void)
{
	return adi_sf_default_speed;
}

unsigned int spl_spi_get_default_bus(void)
{
	return adi_sf_default_bus;
}

unsigned int spl_spi_get_default_cs(void)
{
	return adi_sf_default_cs;
}

void board_boot_order(u32 *spl_boot_list)
{
	const char *bmodestring = sc5xx_get_boot_mode(&bmode);
	printf("ADI Boot Mode: 0x%x (%s)\n", bmode, bmodestring);

	if (bmode != 0 && spl_start_uboot()) {
		spl_boot_list[0] = BOOT_DEVICE_BOOTROM;
		return;
	}

	switch (bmode) {
	case 0:
		printf("SPL execution has completed.  Please load U-Boot Proper via JTAG");
		while (1)
			;
	case 1:
		adi_sf_default_bus = 2;
		adi_sf_default_cs = 1;
		spl_boot_list[0] = BOOT_DEVICE_SPI;
		break;
	case 5:
		adi_sf_default_bus = 0;
		adi_sf_default_cs = 0;
		spl_boot_list[0] = BOOT_DEVICE_SPI;
		break;
	case 6:
		spl_boot_list[0] = BOOT_DEVICE_MMC1;
		break;
	default:
		spl_boot_list[0] = BOOT_DEVICE_BOOTROM;
		break;
	}
}

int32_t __weak adi_rom_boot_hook(struct ADI_ROM_BOOT_CONFIG *config, int32_t cause)
{
	return 0;
}

int board_return_to_bootrom(struct spl_image_info *spl_image,
			    struct spl_boot_device *bootdev)
{
#if CONFIG_ADI_SPL_FORCE_BMODE != 0
	// see above
	if (bmode != 0 && bmode != 3)
		bmode = CONFIG_ADI_SPL_FORCE_BMODE;
#endif

	if (bmode >= (ARRAY_SIZE(adi_rom_boot_args)))
		bmode = 0;

	adi_rom_boot((void *)adi_rom_boot_args[bmode].addr,
		     adi_rom_boot_args[bmode].flags,
		     0, &adi_rom_boot_hook,
		     adi_rom_boot_args[bmode].cmd);
	return 0;
};

void board_init_f(ulong dummy)
{
	int ret;

	clks_init();
	DMC_Config();
	sc5xx_soc_init();

	ret = spl_early_init();
	if (ret)
		panic("spl_early_init() failed\n");

	preloader_console_init();
}

void spl_board_prepare_for_linux(void)
{
	void *fdt = (void *)CONFIG_SPL_PAYLOAD_ARGS_ADDR;
	int nodeoff;
	int ret;
	char bootargs[512];
	char adi_bootargs[256];
	const char *mode_bootargs = NULL;

	switch (bmode) {
	case 1:
		mode_bootargs = "rootfstype=ubifs root=ubi0:rootfs ubi.mtd=3 rw";
		break;
	case 5:
		mode_bootargs = "rootfstype=ubifs root=ubi0:rootfs ubi.mtd=3 rw";
		break;
	case 6:
		/* eMMC: matches the board env "mmcargs" */
		mode_bootargs = "root=/dev/mmcblk0p2 rw rootfstype=ext4 rootwait";
		break;
	default:
		break;
	}

	if (!mode_bootargs)
		return;

	ret = snprintf(adi_bootargs, sizeof(adi_bootargs),
#if defined(CONFIG_SC59X_64)
		       "earlycon=adi_uart,0x31003000 "
		       "console=ttySC0,%d vmalloc=512M",
		       CONFIG_BAUDRATE);
#else
		       "earlyprintk=serial,uart0,%d "
		       "console=ttySC0,%d vmalloc=512M",
		       CONFIG_BAUDRATE, CONFIG_BAUDRATE);
#endif
	if (ret <= 0 || ret >= sizeof(adi_bootargs))
		return;

	ret = snprintf(bootargs, sizeof(bootargs), "%s %s",
		       mode_bootargs, adi_bootargs);
	if (ret <= 0 || ret >= sizeof(bootargs))
		return;

	ret = fdt_check_header(fdt);
	if (ret)
		return;

	nodeoff = fdt_path_offset(fdt, "/chosen");
	if (nodeoff < 0)
		nodeoff = fdt_add_subnode(fdt, 0, "chosen");
	if (nodeoff < 0)
		return;

	fdt_setprop_string(fdt, nodeoff, "bootargs", bootargs);
	printf("SC5xx SPL bootargs: %s\n", bootargs);
}

void *board_spl_fit_buffer_addr(ulong fit_size, int sectors, int bl_len)
{
	return (void *)CONFIG_SPL_LOAD_FIT_ADDRESS;
}
