/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2023 - Analog Devices, Inc.
 *
 */

#include <clk.h>
#include <dm.h>
#include <generic-phy.h>
#include <sdhci.h>
#include <mmc.h>
#include <linux/delay.h>

/* MMC Boot frequency */
#define SDHCI_BOOT_CLK_RATE_HZ                   (400U * 1000U)

/* Vendor register offsets */
#define SDHCI_VENDOR1_MSHC_CTRL_R_OFF            (0x508U)
#define SDHCI_VENDOR1_EMMC_CTRL_R_OFF            (0x52CU)
#define SDHCI_VENDOR1_AT_CTRL_R_OFF              (0x540U)

/* Vendor register field bit masks */
#define SDHCI_VENDOR1_NEGEDGE_DATAOUT_EN         BIT(1)
#define SDHCI_VENDOR1_ENH_STROBE_EN_BM           BIT(8)
#define SDHCI_VENDOR1_CARD_IS_EMMC               BIT(0)

/* Vendor register field values */
#define POST_CHANGE_DLY_LESS_4_CYCLES            (0x03U)
#define TUNE_CLK_STOP_EN                         (1U)

/* Vendor register Bit positions */
#define POST_CHANGE_DLY_OFF                      (19U)
#define TUNE_CLK_STOP_EN_OFF                     (16U)

/* CTRL reg pll enable */
#define SDHCI_CLOCK_PLL_EN                       BIT(3)

/* Max tuning iterations */
#define SDHCI_MAX_TUNING_LOOP                    (128)

/* Workaround for non-standard definition in sdhci.h */
#define SDHCI_CTRL_HS400_STD                     (0x0007)

/* SDHCI PHY configure operations */
#define SDHCI_PHY_OPS_CFG_DLL_NO_CLK             (1U)
#define SDHCI_PHY_OPS_ENABLE_DLL_AFTER_CLK       (2U)
#define SDHCI_PHY_OPS_SET_DELAY                  (3U)

/* To this day, ADI design integrates the 1.8V IP versions (Host Controller
 * and PHY for eMMC and Host Controller for SD). All eMMC speed modes are
 * supported and all modes can operate at 1.8V. On the other side, only low speed
 * modes are supported in SD card and those modes can only operate at 3.3V. That
 * is solved by adding an externel level shifter.
 * Driver does not manage well this SD scenario.
 * Note: This macro is just to identify the action taken on this issue
 */
#define SDHCI_ADI_IP_1_8V

/* PHY Delay Lines may cause a potential glitch on the RX clock (because PHY DL2
 * input (rx clock) is connected to PHY DL1 output (tx clock)). Delay lines
 * configuration comes from Synopsys, and.it is expected not to change for future
 * products.
 * Note: This macro is just to identify the action taken on this issue
 */
#define SDHCI_ADI_RX_CLOCK_GLITCH

/* MMC HS400 in Adrv906x requires data to be sent out on negedge of cclk_tx.
 * This is a soc-specific requirement (coming from Adrv906x GLS simulations).
 */
#define SDHCI_ADI_HS400_TX_CLK_NEGEDGE

/* According to the 'DesignWare Cores MSHC' User Guide, if there is an external
 * clock multiplexor, it could cause glitches on cclk_rx.
 * Stop the card clock before setting the tuning bit (EXEC_TUNING) and restart
 * it after it.
 * Note: option untested
 *
 * #define SDHCI_ADI_TUNING_RX_CLOCK_GLITCH
 */

#define SDHCI_IDLE_TIMEOUT                       (20) /* 20 ms */

struct adi_sdhc_plat {
#if CONFIG_IS_ENABLED(OF_PLATDATA)
	struct dtd_adi_sdhci dtplat;
#endif
	struct mmc_config cfg;
	struct mmc mmc;
};

/* Host Controller - PHY interface */
struct adi_phy_opts {
	u8 event;
	u8 arg1;
};

struct adi_sdhc {
	struct sdhci_host host;
	struct phy phy;
	void *base;
};


bool is_protium(void);
bool is_palladium(void);
static int adi_sdhci_set_delay(struct sdhci_host *host);

static int adi_sdhci_setup_phy(struct udevice *dev)
{
	struct adi_sdhc *priv = dev_get_priv(dev);
	int ret;

	ret = generic_phy_get_by_index(dev, 0, &priv->phy);
	if (ret) {
		if (ret == -ENOENT)
			return 0;
		printf("Failed to get SDHCI PHY: %d.\n", ret);
		return ret;
	}

	ret = generic_phy_init(&priv->phy);
	if (ret) {
		printf("Failed to init SDHCI PHY: %d.\n", ret);
		return ret;
	}

	return 0;
}

static int adi_sdhci_set_clock(struct sdhci_host *host, u32 clock)
{
	unsigned int div, clk = 0, timeout;

	/* Workaround: ADRV906X does not use SDHCI_DIVIDER as specified
	 * in the SDHCI spec. Instead of 1/(2N), it is 1/(N+1). This function
	 * is basically a copy of sdhci_set_clock(), but with the updated
	 * divider calculation.
	 */
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (host->max_clk <= clock) {
		div = 0;
	} else {
		div = (host->max_clk / clock) - 1U;
		if ((host->max_clk % clock) != 0)
			div++;
	}

	clk |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
		 & SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			printf("%s: Internal clock never stabilised.\n",
			       __func__);
			return -EBUSY;
		}
		timeout--;
		udelay(1000);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	return 0;
}

#if CONFIG_IS_ENABLED(MMC_HS400_SUPPORT)
static int adi_sdhci_config_dll(struct sdhci_host *host, u32 clock, bool enable)
{
	struct adi_sdhc *priv = dev_get_priv(host->mmc->dev);
	struct adi_phy_opts phy_opts;
	int ret = 0;

	/* Workaround: ADRV906X has a custom SDHCI_DIVIDER. Setting this in
	 * config_dll() allows us to override the divider logic in
	 * sdhci_set_clock().
	 */
	if (enable) {
		ret = adi_sdhci_set_clock(host, clock);
		if (ret != 0)
			return ret;
	}

	/* DLL configured only for HS400 and HS400ES modes */
	if (host->mmc->selected_mode != MMC_HS_400 &&
	    host->mmc->selected_mode != MMC_HS_400_ES)
		/* Disable here DLL */
		return 0;

	if (enable == false)
		phy_opts.event = SDHCI_PHY_OPS_CFG_DLL_NO_CLK;
	else
		phy_opts.event = SDHCI_PHY_OPS_ENABLE_DLL_AFTER_CLK;

	return generic_phy_configure(&priv->phy, &phy_opts);
}

static int adi_sdhci_set_enhanced_strobe(struct sdhci_host *host)
{
	u16 emmc_ctrl;

	emmc_ctrl = sdhci_readw(host, SDHCI_VENDOR1_EMMC_CTRL_R_OFF);
	emmc_ctrl |= SDHCI_VENDOR1_ENH_STROBE_EN_BM;
	sdhci_writew(host, emmc_ctrl, SDHCI_VENDOR1_EMMC_CTRL_R_OFF);

	return 0;
}
#else
static int adi_sdhci_config_dll(struct sdhci_host *host, u32 clock, bool enable)
{
	int ret = 0;

	/* Workaround: ADRV906X has a custom SDHCI_DIVIDER. Setting this in
	 * config_dll() allows us to override the divider logic in
	 * sdhci_set_clock().
	 */
	if (enable)
		ret = adi_sdhci_set_clock(host, clock);

	return ret;
}
#endif

#ifdef SDHCI_ADI_RX_CLOCK_GLITCH
static void adi_sdhci_fix_rx_clock_glitch(struct sdhci_host *host, enum bus_mode mode)
{
	u32 reg;

	/* This configuration helps to fix this issue (verified in RTL and GLS simulations) */
	if ((mode == MMC_HS_400_ES) || (mode == MMC_HS_400) || (mode == MMC_HS_200))
		reg = (POST_CHANGE_DLY_LESS_4_CYCLES << POST_CHANGE_DLY_OFF);
	else
		reg = (POST_CHANGE_DLY_LESS_4_CYCLES << POST_CHANGE_DLY_OFF) |
		      (TUNE_CLK_STOP_EN << TUNE_CLK_STOP_EN_OFF);

	sdhci_writel(host, reg, SDHCI_VENDOR1_AT_CTRL_R_OFF);
}
#endif

static void adi_sdhci_set_clk_tx_negedge(struct sdhci_host *host, bool enable)
{
	u16 mshc_ctrl;

	mshc_ctrl = sdhci_readw(host, SDHCI_VENDOR1_MSHC_CTRL_R_OFF);
	if (enable)
		mshc_ctrl |= SDHCI_VENDOR1_NEGEDGE_DATAOUT_EN;
	else
		mshc_ctrl &= ~SDHCI_VENDOR1_NEGEDGE_DATAOUT_EN;
	sdhci_writew(host, mshc_ctrl, SDHCI_VENDOR1_MSHC_CTRL_R_OFF);
}

static int adi_sdhci_set_delay(struct sdhci_host *host)
{
	struct mmc *mmc = host->mmc;
	struct adi_sdhc *priv = dev_get_priv(host->mmc->dev);
	struct adi_phy_opts phy_opts;

#ifdef SDHCI_ADI_RX_CLOCK_GLITCH
	/* eMMC PHY delay lines may cause a glitch on RX clock */
	adi_sdhci_fix_rx_clock_glitch(host, mmc->selected_mode);
#endif

	phy_opts.event = SDHCI_PHY_OPS_SET_DELAY;
	phy_opts.arg1 = mmc->selected_mode;

	return generic_phy_configure(&priv->phy, &phy_opts);
}

#ifdef SDHCI_ADI_IP_1_8V
static int adi_sdhci_set_voltage(struct sdhci_host *host)
{
	u32 reg;

	/* Workaround:
	 * ADRV906X requirement is to set SIGNALING_EN always to 1 (Host
	 * Controller always work at 1.8V).
	 * There is no easy support on the general driver for that:
	 * - During initialization, first it tries to set 3.3V, If it fails, it
	 *   tries 1.8V
	 * - When switching to low speed modes, it tries only 3.3V
	 *
	 * Set unconditionally signalling to 1.8V (ADRV906X specific workaround)
	 * regardless the requested voltage
	 */
	reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	reg |= SDHCI_CTRL_VDD_180;
	sdhci_writew(host, reg, SDHCI_HOST_CONTROL2);

	return 0;
}
#endif

static int adi_sdhci_set_uhs_timing(struct sdhci_host *host)
{
	struct mmc *mmc = host->mmc;
	u32 reg;

	/* Workaround: general framework uses a wrong HS400 definition */
	if ((mmc->selected_mode == MMC_HS_400) ||
	    (mmc->selected_mode == MMC_HS_400_ES)) {
		reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		reg &= ~SDHCI_CTRL_UHS_MASK;
		reg |= SDHCI_CTRL_HS400_STD;
		sdhci_writew(host, reg, SDHCI_HOST_CONTROL2);

#ifdef SDHCI_ADI_HS400_TX_CLK_NEGEDGE
		adi_sdhci_set_clk_tx_negedge(host, true);
#else
		adi_sdhci_set_clk_tx_negedge(host, false);
#endif
	} else {
		sdhci_set_uhs_timing(host);

		adi_sdhci_set_clk_tx_negedge(host, false);
	}

	return 0;
}

static void adi_sdhci_set_control_reg(struct sdhci_host *host)
{
	/* Ideally, sdhci_set_control_reg (sdhci.c) should be used, but:
	 * - sdhci_set_voltage applies (write to SIGNALING_EN) only to SD (weird)
	 * - sdhci_set_uhs_timing uses a wrong HS400 definition
	 */
#ifdef SDHCI_ADI_IP_1_8V
	adi_sdhci_set_voltage(host);
#endif
	adi_sdhci_set_uhs_timing(host);
}

#if CONFIG_IS_ENABLED(MMC_HS200_SUPPORT)
/* This function is a copy&paste from sdhci_reset() (sdhci.c) */
static void adi_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	unsigned long timeout;

	/* Wait max 100 ms */
	timeout = 100;
	sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);
	while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			printf("%s: Reset 0x%x never completed.\n",
			       __func__, (int)mask);
			return;
		}
		timeout--;
		udelay(1000);
	}
}

static int adi_sdhci_platform_execute_tuning(struct mmc *mmc, u8 opcode)
{
	struct sdhci_host *host = dev_get_priv(mmc->dev);
	struct mmc_cmd cmd;
	u16 ctrl;
	u32 blk_size;
	int loop_cnt = 0;
	int ret = 0;

	/* Synopsys eMMC PHY delay lines code is not synthetizable for
	 * protium/palladium, so tuning sequence procedure is not supported */
	if ((is_protium() || is_palladium()))
		return 0;

	/* clock tuning is not needed for upto 52MHz */
	if (!((mmc->selected_mode == MMC_HS_200) ||
	      (mmc->selected_mode == MMC_HS_400) ||
	      (mmc->selected_mode == UHS_SDR104) ||
	      (mmc->selected_mode == UHS_SDR50)))
		return 0;

	/*
	 * Tuning sequence (DesignWare Cores mshc User Guide)
	 */

	/* Rx clk tuning procedure is only based on BUF_RD_READY status bit */
	sdhci_writel(host, SDHCI_INT_DATA_AVAIL, SDHCI_INT_ENABLE);
	sdhci_writel(host, SDHCI_INT_DATA_AVAIL, SDHCI_SIGNAL_ENABLE);

#if SDHCI_ADI_TUNING_RX_CLOCK_GLITCH
	ctrl = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	ctrl &= ~SDHCI_CLOCK_CARD_EN;
#endif

	/* Set EXEC_TUNING to 1 */
	ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	ctrl |= SDHCI_CTRL_EXEC_TUNING;
	sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

#if SDHCI_ADI_TUNING_RX_CLOCK_GLITCH
	ctrl = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	ctrl |= SDHCI_CLOCK_CARD_EN;
#endif

	/* CMD21 (MMC_CMD_SEND_TUNING_BLOCK_HS200) is similar to a read command,
	 * so the appropriate values for BLOCKSIZE_R, BLOCK_COUNT_R and
	 * XFERMOD_R must be set
	 */
	cmd.cmdidx = opcode;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = 0;

	if (opcode == MMC_CMD_SEND_TUNING_BLOCK_HS200 && host->mmc->bus_width == 8)
		blk_size = SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG, 128);
	else
		blk_size = SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG, 64);

	sdhci_writew(host, blk_size, SDHCI_BLOCK_SIZE);
	sdhci_writew(host, 1, SDHCI_BLOCK_COUNT);
	sdhci_writew(host, SDHCI_TRNS_READ, SDHCI_TRANSFER_MODE);

	do {
		if (++loop_cnt > SDHCI_MAX_TUNING_LOOP) {
			ctrl &= ~SDHCI_CTRL_EXEC_TUNING;
			sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
			ret = -ECOMM;
			break;
		}

		mmc_send_cmd(mmc, &cmd, NULL);
		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	} while (ctrl & SDHCI_CTRL_EXEC_TUNING);

	/* Check if SAMPLE_CLK_SEL == 1 */
	if (0 == (ctrl & SDHCI_CTRL_TUNED_CLK))
		ret = -ECOMM;

	if (ret < 0) {
		/* Tuning error recovery */
		ctrl &= ~SDHCI_CTRL_TUNED_CLK;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
		adi_sdhci_reset(host, SDHCI_RESET_CMD);
		adi_sdhci_reset(host, SDHCI_RESET_DATA);
	}

	/* Re-enable interrupts after doing rx clk tuning procedure */
	sdhci_writel(host, SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK,
		     SDHCI_INT_ENABLE);
	sdhci_writel(host, 0x0, SDHCI_SIGNAL_ENABLE);

	return ret;
}
#endif

static const struct sdhci_ops adi_sdhci_emmc_ops = {
	.set_control_reg		= adi_sdhci_set_control_reg,
	.set_delay			= adi_sdhci_set_delay,
#if CONFIG_IS_ENABLED(MMC_HS200_SUPPORT)
	.platform_execute_tuning	= adi_sdhci_platform_execute_tuning,
#endif
#if CONFIG_IS_ENABLED(MMC_HS400_SUPPORT) || CONFIG_IS_ENABLED(MMC_HS400_ES_SUPPORT)
	.config_dll			= adi_sdhci_config_dll,
#endif
#if CONFIG_IS_ENABLED(MMC_HS400_ES_SUPPORT)
	.set_enhanced_strobe		= adi_sdhci_set_enhanced_strobe
#endif
};

static const struct sdhci_ops adi_sdhci_sd_ops = {
	.config_dll	= adi_sdhci_config_dll
};

static int adi_sdhci_deinit(struct sdhci_host *host)
{
	uint16_t u16_reg_data;
	unsigned int timeout = SDHCI_IDLE_TIMEOUT;

	/* Wait for the host controller to become idle before stopping card clock.*/
	while (sdhci_readl(host, SDHCI_PRESENT_STATE) &
	       (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
		if (--timeout == 0) {
			printf("Host controller is not idle\n");
			return -EBUSY;
		}
		udelay(1000);
	}

	/* Stop card clock, and turn off internal clock and PLL */
	u16_reg_data = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	u16_reg_data &= ~(SDHCI_CLOCK_CARD_EN | SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_PLL_EN);
	sdhci_writew(host, u16_reg_data, SDHCI_CLOCK_CONTROL);

	return 0;
}

static int adi_sdhci_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct adi_sdhc_plat *plat = dev_get_plat(dev);
	struct adi_sdhc *prv = dev_get_priv(dev);
	struct sdhci_host *host = &prv->host;
	int max_frequency, ret;
	bool init_phy;
	bool is_emmc;
	bool is_no_1_8v;
	struct clk clk;
	unsigned long clock;

#if CONFIG_IS_ENABLED(OF_PLATDATA)
	struct dtd_adi_sdhci *dtplat = &plat->dtplat;

	host->name = dev->name;
	host->ioaddr = map_sysmem(dtplat->reg[0], dtplat->reg[1]);
	max_frequency = dtplat->max_frequency;
	is_emmc = dtplat->non_removable;
	is_no_1_8v = dtplat->no_1_8_v;
	init_phy = dtplat->init_phy;
	ret = clk_get_by_driver_info(dev, dtplat->clocks, &clk);
#else
	max_frequency = dev_read_u32_default(dev, "max-frequency", 0);
	init_phy = dev_read_bool(dev, "enable-phy-config");
	is_emmc = dev_read_bool(dev, "non-removable");
	is_no_1_8v = dev_read_bool(dev, "no-1-8-v");
	ret = clk_get_by_index(dev, 0, &clk);
#endif

	clock = clk_get_rate(&clk);
	host->quirks = 0;
	host->max_clk = clock;
	plat->cfg.f_max = max_frequency;

	/* Deinit to ensure a proper initialization */
	ret = adi_sdhci_deinit(host);
	if (ret)
		return ret;
	/*
	 * The sdhci-driver only supports 4bit and 8bit, as sdhci_setup_cfg
	 * doesn't allow us to clear MMC_MODE_4BIT.  Consequently, we don't
	 * check for other bus-width values.
	 */
	if (host->bus_width == 8)
		host->host_caps |= MMC_MODE_8BIT;

	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;

	host->mmc = &plat->mmc;
	host->mmc->priv = &prv->host;
	host->mmc->dev = dev;
	upriv->mmc = host->mmc;

	if (is_emmc) {
		host->ops = &adi_sdhci_emmc_ops;
		if (is_no_1_8v)
			/* General framework enables HS200 even if "no-1-8-v"
			 * property is present in device tree as long as both
			 * the card and the host support it, This quirk allows
			 * to disable HSx00 modes.*/
			host->quirks |= SDHCI_QUIRK_NO_1_8_V;
	} else {
		/* As per Spec, Host System should set Voltage support to 3.3V
		 * or 3.0V for SD card. But, ADI drives 1.8V and level shifter
		 * in the board converts to 3.3V */
		host->voltages = MMC_VDD_32_33 | MMC_VDD_33_34;

		host->quirks |= SDHCI_QUIRK_BROKEN_VOLTAGE;

		host->ops = &adi_sdhci_sd_ops;
	}

	ret = sdhci_setup_cfg(&plat->cfg, host, plat->cfg.f_max, SDHCI_BOOT_CLK_RATE_HZ);
	if (ret)
		return ret;

	ret = sdhci_probe(dev);
	if (ret)
		return ret;

	if (is_emmc) {
		int16_t emmc_ctrl;

		/* Set CARD_IS_EMMC bit */
		emmc_ctrl = sdhci_readw(host, SDHCI_VENDOR1_EMMC_CTRL_R_OFF);
		if (!(emmc_ctrl & SDHCI_VENDOR1_CARD_IS_EMMC)) {
			emmc_ctrl |= SDHCI_VENDOR1_CARD_IS_EMMC;
			sdhci_writew(host, emmc_ctrl, SDHCI_VENDOR1_EMMC_CTRL_R_OFF);
		}

#ifdef SDHCI_ADI_RX_CLOCK_GLITCH
		/* eMMC PHY delay lines may cause a glitch on RX clock */
		adi_sdhci_fix_rx_clock_glitch(host, MMC_LEGACY);
#endif
	}

	if (init_phy)
		return adi_sdhci_setup_phy(dev);
	else
		return 0;
}

static int adi_sdhci_of_to_plat(struct udevice *dev)
{
#if !CONFIG_IS_ENABLED(OF_PLATDATA)
	struct sdhci_host *host = dev_get_priv(dev);

	host->name = dev->name;
	host->ioaddr = dev_read_addr_ptr(dev);
	host->bus_width = dev_read_u32_default(dev, "bus-width", 4);
#endif

	return 0;
}

static int adi_sdhci_bind(struct udevice *dev)
{
	struct adi_sdhc_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id adi_sdhci_ids[] = {
	{ .compatible = "adi,sdhci" },
	{ }
};

U_BOOT_DRIVER(adi_adrv906x_sdhci) = {
	.name		= "adi_adrv906x_sdhci",
	.id		= UCLASS_MMC,
	.of_match	= adi_sdhci_ids,
	.of_to_plat	= adi_sdhci_of_to_plat,
	.ops		= &sdhci_ops,
	.bind		= adi_sdhci_bind,
	.probe		= adi_sdhci_probe,
	.priv_auto	= sizeof(struct adi_sdhc),
	.plat_auto	= sizeof(struct adi_sdhc_plat),
};
