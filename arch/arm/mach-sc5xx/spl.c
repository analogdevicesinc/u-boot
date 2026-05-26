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
#include <asm/arch/sc5xx_sharc_idle.h>
#include "init/clkinit.h"
#ifdef CONFIG_SC846
#include "init/EHP2_LP4_1D_2000/EHP2_LPDDR4_1D_2000_Core1.h"
#endif

#if (IS_ENABLED(CONFIG_SC5XX_DMC_INIT))
#include "init/dmcinit.h"
#endif

static bool adi_start_uboot_proper;

static int adi_sf_default_bus = CONFIG_SF_DEFAULT_BUS;
static int adi_sf_default_cs = CONFIG_SF_DEFAULT_CS;
static int adi_sf_default_speed = CONFIG_SF_DEFAULT_SPEED;

u32 bmode;

int spl_start_uboot(void)
{
	return adi_start_uboot_proper;
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

	/*
	 * By default everything goes back to the bootrom, where we'll read table
	 * parameters and ask for another image to be loaded
	 */
	spl_boot_list[0] = BOOT_DEVICE_BOOTROM;

	if (bmode == 0) {
		printf("SPL execution has completed.  Please load U-Boot Proper via JTAG");
		while (1)
			;
	}
}

int32_t __weak adi_rom_boot_hook(struct ADI_ROM_BOOT_CONFIG *config, int32_t cause)
{
	return 0;
}

int board_return_to_bootrom(struct spl_image_info *spl_image,
			    struct spl_boot_device *bootdev)
{

#if CONFIG_ADI_SPL_FORCE_BMODE
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
#if (IS_ENABLED(CONFIG_SC5XX_DMC_INIT))
	DMC_Config();
#endif
	sc5xx_soc_init();

	ret = spl_early_init();
	if (ret)
		panic("spl_early_init() failed\n");

	preloader_console_init();

#if (IS_ENABLED(CONFIG_SC846))
	printf("clks_init: Starting LPDDR4 initialization\n");
	pre_reset_init_lpddr4();
	printf("clks_init: Between pre and post reset\n");
	ret = post_reset_init_lpddr4();
	if (ret) {
		printf("clks_init: FATAL - LPDDR4 initialization failed!\n");
		panic("DDR initialization failed, cannot boot\n");
	}
	printf("clks_init: LPDDR4 initialization complete\n");
#endif
#ifdef CONFIG_SET_SHARC_IDLE
	ret = set_sharc_cores_idle();
	if (ret)
		printf("Warn: failed to set SHARC cores idle (%d)\n", ret);
#endif
}

