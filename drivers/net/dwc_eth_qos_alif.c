// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Glue for the Synopsys DWC Ethernet QoS block of the Alif Ensemble SoCs.
 */

#include <dm.h>
#include <env.h>
#include <net.h>
#include <phy.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/kernel.h>

#include "dwc_eth_qos.h"

/* Peripheral clock control, slave and master side */
#define ALIF_CCPSLV_BASE		0x4902f000
#define ALIF_CCPMST_BASE		0x4903f000
#define ALIF_CCPSLV_PCLK_FORCE		BIT(30)
#define ALIF_CCPSLV_IPCLK_FORCE		BIT(31)
#define ALIF_CCPMST_ETH_CSR_CLK_EN	0x0c
#define  ALIF_ETH_CSR_CLK_EN		BIT(12)
#define ALIF_CCPMST_ETH_CLK_SEL		0x80
#define  ALIF_ETH_CLK_SEL_INTERNAL	BIT(4)

/* syst_hclk, the CSR clock of the MAC, is PLL_CLK1 / 4 */
#define ALIF_ETH_CSR_CLK_RATE		200000000

static int eqos_probe_resources_alif(struct udevice *dev)
{
	phy_interface_t interface;
	int ret;

	ret = eqos_get_base_addr_dt(dev);
	if (ret) {
		pr_err("eqos_get_base_addr_dt failed: %d\n", ret);
		return ret;
	}

	interface = dev_read_phy_mode(dev);
	if (interface != PHY_INTERFACE_MODE_RMII) {
		pr_err("Only RMII is supported\n");
		return -EINVAL;
	}

	return 0;
}

static int eqos_start_clks_alif(struct udevice *dev)
{
	/* Force the peripheral functional and APB clocks on */
	writel(ALIF_CCPSLV_PCLK_FORCE | ALIF_CCPSLV_IPCLK_FORCE,
	       ALIF_CCPSLV_BASE);

	setbits_le32(ALIF_CCPMST_BASE + ALIF_CCPMST_ETH_CSR_CLK_EN,
		     ALIF_ETH_CSR_CLK_EN);

	/* By default the 50 MHz reference clock is provided by the PHY */
	if (dev_read_bool(dev, "alif,internal-refclk"))
		setbits_le32(ALIF_CCPMST_BASE + ALIF_CCPMST_ETH_CLK_SEL,
			     ALIF_ETH_CLK_SEL_INTERNAL);
	else
		clrbits_le32(ALIF_CCPMST_BASE + ALIF_CCPMST_ETH_CLK_SEL,
			     ALIF_ETH_CLK_SEL_INTERNAL);

	return 0;
}

static ulong eqos_get_tick_clk_rate_alif(struct udevice *dev)
{
	return ALIF_ETH_CSR_CLK_RATE;
}

static int eqos_get_enetaddr_alif(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);

	return eth_env_get_enetaddr("ethaddr", pdata->enetaddr);
}

static struct eqos_ops eqos_alif_ops = {
	.eqos_inval_desc = eqos_inval_desc_generic,
	.eqos_flush_desc = eqos_flush_desc_generic,
	.eqos_inval_buffer = eqos_inval_buffer_generic,
	.eqos_flush_buffer = eqos_flush_buffer_generic,
	.eqos_probe_resources = eqos_probe_resources_alif,
	.eqos_remove_resources = eqos_null_ops,
	.eqos_stop_resets = eqos_null_ops,
	.eqos_start_resets = eqos_null_ops,
	.eqos_stop_clks = eqos_null_ops,
	.eqos_start_clks = eqos_start_clks_alif,
	.eqos_calibrate_pads = eqos_null_ops,
	.eqos_disable_calibration = eqos_null_ops,
	.eqos_set_tx_clk_speed = eqos_null_ops,
	.eqos_get_enetaddr = eqos_get_enetaddr_alif,
	.eqos_get_tick_clk_rate = eqos_get_tick_clk_rate_alif,
};

struct eqos_config __maybe_unused eqos_alif_config = {
	.reg_access_always_ok = true,
	.mdio_wait = 20,
	.swr_wait = 50,
	.config_mac = EQOS_MAC_RXQ_CTRL0_RXQ0EN_ENABLED_DCB,
	.config_mac_mdio = EQOS_MAC_MDIO_ADDRESS_CR_150_250,
	.axi_bus_width = EQOS_AXI_WIDTH_32,
	.interface = dev_read_phy_mode,
	.ops = &eqos_alif_ops,
};
