// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 */

#include <spl.h>
#include <adi_spi3.h>
#include <asm/arch/sc5xx.h>
#include <asm/arch/spl.h>
#include <asm/io.h>
#include "init/clkinit.h"
#include "init/dmcinit.h"

#if IS_ENABLED(CONFIG_SC59X) || IS_ENABLED(CONFIG_SC59X_64)
#define SC5XX_SPI2_BASE		0x31030000
#elif IS_ENABLED(CONFIG_SC57X) || IS_ENABLED(CONFIG_SC58X)
#define SC5XX_SPI2_BASE		0x31044000
#else
#error "Unknown SC5XX SOC"
#endif

#define BMODE_NOBOOT		0
#define BMODE_SPI_CTLR		1
#define BMODE_SPI_TARGET	2
#define BMODE_UART		3
#define BMODE_LINKPORT		4
#define BMODE_OSPI		5
#define BMODE_EMCC		6

#define ROM_BCMD_SPIM_ADDR_24BIT (2 << 20)
#define ROM_BCMD_SPIM_ADDR_32BIT (3 << 20)

#define ISSI_BAR_EXTADD_BIT	BIT(7)

#if IS_ENABLED(CONFIG_TARGET_SC598_SOM_EZKIT)
#define SPI_FLASH_ISSI
#else
#error "Unknown SC5XX board"
#endif

#ifdef SPI_FLASH_ISSI
/* RDBR: Read volatile Bank Address Register (ISSI-specific, opcode 0x16) */
static u8 spi_nor_read_bar_issi(void)
{
	struct adi_spi_regs __iomem *regs = (struct adi_spi_regs __iomem *)SC5XX_SPI2_BASE;
	u32 v;

	v = readl(&regs->ssel);
	/* Ensure CS starts deasserted */
	v |= BIT_SSEL_VAL(CONFIG_SC_BOOT_SPI_SSEL);
	v |= BIT_SSEL_EN(CONFIG_SC_BOOT_SPI_SSEL);
	writel(v, &regs->ssel);

	writel(0x10, &regs->clock);
	writel(SPI_CTL_EN | SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_ASSEL, &regs->control);

	writel(SPI_RXCTL_REN, &regs->rx_control);
	writel(SPI_TXCTL_TEN | SPI_TXCTL_TTI, &regs->tx_control);

	writel(0x16, &regs->tfifo);  /* RDBR opcode */
	writel(0xff, &regs->tfifo);  /* Dummy byte to clock in BAR */

	v &= ~BIT_SSEL_VAL(CONFIG_SC_BOOT_SPI_SSEL);
	writel(v, &regs->ssel);

	while (readl(&regs->status) & SPI_STAT_RFE)
		;
	readl(&regs->rfifo);         /* Discard */

	while (readl(&regs->status) & SPI_STAT_RFE)
		;
	v = readl(&regs->rfifo);     /* BAR value */

	/* Deassert CS between transactions */
	v |= BIT_SSEL_VAL(CONFIG_SC_BOOT_SPI_SSEL);
	writel(v, &regs->ssel);

	return (u8)v;
}
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

	if (bmode == BMODE_NOBOOT) {
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
	u32 bar, cmd;

#if CONFIG_ADI_SPL_FORCE_BMODE != 0
	// see above
	if (bmode != BMODE_NOBOOT && bmode != BMODE_UART)
		bmode = CONFIG_ADI_SPL_FORCE_BMODE;
#endif

	if (bmode >= (ARRAY_SIZE(adi_rom_boot_args)))
		bmode = BMODE_NOBOOT;

	cmd = adi_rom_boot_args[bmode].cmd;

	if (bmode == BMODE_SPI_CTLR) {
#ifdef SPI_FLASH_ISSI
		bar = spi_nor_read_bar_issi();

		printf("ISSI SPI NOR: %s addressing mode (BAR=0x%02x)\n",
		       (bar & ISSI_BAR_EXTADD_BIT) ? "4-byte" : "3-byte", bar);

		if (bar & ISSI_BAR_EXTADD_BIT)
			cmd |= ROM_BCMD_SPIM_ADDR_32BIT;
		else
			cmd |= ROM_BCMD_SPIM_ADDR_24BIT;
#else
#error "Unable to detect SPI flash addressing mode"
#endif
	}

	adi_rom_boot((void *)adi_rom_boot_args[bmode].addr,
		     adi_rom_boot_args[bmode].flags,
		     0, &adi_rom_boot_hook, cmd);

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

