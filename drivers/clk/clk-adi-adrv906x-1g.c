/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2023 - Analog Devices, Inc.
 *
 * Some caveats:
 * - Currently only supports RGMII targets (as its dwmac4 driver counterpart)
 */

#include <common.h>
#include <clk.h>
#include <clk-uclass.h>
#include <dm.h>
#include <log.h>
#include <linux/types.h>
#include <linux/clk-provider.h>
#include <linux/io.h>

/* Description
 *
 * Commonly, other ADI products (ie: SC584) provide a fixed 125 MHz clock to the
 * 1G Eth Synopsys IP, and the IP itself derives the required clock (2.5|25|125
 * MHz) based on the PHY link speed (10|100|1000).
 * ADRV906X design includes a HW divider block between the input clock the GMAC IP,
 * and the IP ability to derive the clock is disabled. The reason is twofold:
 * - External input may be 250 MHz, so at the very least, the clock needs to be
 *   divided by 2
 * - ADRV906X supports both RMII and RGMII. That implies more flexibility on the
 *   clock generation than on the other products just supporting a single
 *   interface type (for more details, talk to HW team).
 *
 * This driver manages this HW divider block, which is responsible for generating
 * the required clock signals for the GMAC and external PHY
 *
 *   CLOCK          |  RMII (MHz)  |  RGMII (MHz)
 *   -------------------------------------------------------
 *   PHY clock      |  50**        |  2.5|25|125*
 *   GMAC Tx clock  |  2.5|25*     |  2.5|25|125*
 *   GMAC Rx clock  |  2.5|25*     |  Provided by PHY device
 *
 *   * depend on PHY Link Speed (10|100|1000)
 *   ** PHY clock in RMII requires a fix 50 MHz clock
 *
 * Clock dividers depend on:
 * - Input clock (ie: 250 MHz)    <- provided via device tree (clock-frequency property)
 * - PHY Link Speed (10|100|1000) <- indirectly provided (2.5|25|125 MHz) via the set_clk_rate API
 * - Phy interface                <- provided via device tree (clock node input argument)
 *
 * Note: this light driver is not based on CCF (Common Clock Framework)
 */

#define LINK_SPEED_10                           10
#define LINK_SPEED_100                          100
#define LINK_SPEED_1000                         1000

#define CLK_SPEED_2_5MHZ                        2500000
#define CLK_SPEED_25MHZ                         25000000
#define CLK_SPEED_50MHZ                         50000000
#define CLK_SPEED_125MHZ                        125000000
#define CLK_SPEED_250MHZ                        250000000

#define EMAC_1G_CG_ENABLE                       BIT(0)
#define EMAC_1G_YODA_MASK                       GENMASK(19, 3)
#define EMAC_1G_YODA_OSC_CLK_DIV_MASK           GENMASK(19, 13)
#define EMAC_1G_YODA_CLK_DIV_MASK               GENMASK(12, 6)
#define EMAC_1G_YODA_PHY_INTF_SEL_I_MASK        GENMASK(5, 3)
#define EMAC_1G_YODA_OSC_CLK_DIV_OFF            13
#define EMAC_1G_YODA_CLK_DIV_OFF                6
#define EMAC_1G_YODA_PHY_INTF_SEL_I_OFF         3
#define EMAC_1G_YODA_PHY_INTF_SEL_I_RMII        4
#define EMAC_1G_YODA_PHY_INTF_SEL_I_RGMII       1


struct adrv906x_priv_data {
	uint32_t base_clk_speed;
	uint32_t phy_interface;
	void __iomem *clk_div_base;
};

struct adrv906x_priv_data sam_priv;

static ulong set_clk_divider(struct adrv906x_priv_data *priv, ulong rate)
{
	uint32_t reg;
	uint32_t iface_type;
	uint32_t osc_div;
	uint32_t rmii_div;


	/* Sanity checks */
	if ((priv->base_clk_speed != CLK_SPEED_50MHZ) &&
	    (priv->base_clk_speed != CLK_SPEED_125MHZ) &&
	    (priv->base_clk_speed != CLK_SPEED_250MHZ)) {
		pr_err("input clock speed not supperted: %d Hz\n", priv->base_clk_speed);
		return -EINVAL;
	}

	if ((priv->base_clk_speed % rate) != 0) {
		pr_err("Unable to set this clock rate (%ld Hz)\n", rate);
		return -EINVAL;
	}

	if ((priv->phy_interface == PHY_INTERFACE_MODE_RMII) &&
	    ((priv->base_clk_speed % CLK_SPEED_50MHZ) != 0)) {
		pr_err("Unable to set RMII PHY clock (%d Hz)\n", CLK_SPEED_50MHZ);
		return -1;
	}

	/* Compute dividers' value */
	switch (priv->phy_interface) {
	/*
	 * input_clk   _|-> OSC_CLK_DIV (50 Mhz) ------> Clk to external PHY
	 * (ie 250MHz)  |-> RMII_CLK_DIV (2.5|25 MHz) -> Tx_clk and Rx_clk to GMAC IP
	 *                  (based on PHY link 10|100)
	 */
	case PHY_INTERFACE_MODE_RMII:
		iface_type = EMAC_1G_YODA_PHY_INTF_SEL_I_RMII;
		osc_div = (priv->base_clk_speed / CLK_SPEED_50MHZ) - 1;
		rmii_div = (priv->base_clk_speed / rate) - 1;
		break;

	/*
	 * input_clk     -> OSC_CLK_DIV (2.5|25|125 MHz) -> Clk to PHY and Tx_clk to GMAC
	 * (ie 250MHz)      (based on PHY link 10|100|1000)
	 *
	 * Note: Rx_clk to GMAC IP is provided by the external PHY
	 * Note: RMII_CLK_DIV does not apply
	 */
	case PHY_INTERFACE_MODE_RGMII:
		iface_type = EMAC_1G_YODA_PHY_INTF_SEL_I_RGMII;
		osc_div = (priv->base_clk_speed / rate) - 1;
		rmii_div = 0;
		break;
	default:
		pr_err("phy mode not supported\n");
		return -1;
	}

	/* Disable clock */
	reg = readl(priv->clk_div_base);
	reg |= EMAC_1G_CG_ENABLE;
	writel(reg, priv->clk_div_base);

	/* Set divider/s */
	reg &= ~EMAC_1G_YODA_MASK;
	reg |= (iface_type << EMAC_1G_YODA_PHY_INTF_SEL_I_OFF) |
	       (osc_div << EMAC_1G_YODA_OSC_CLK_DIV_OFF) |
	       (rmii_div << EMAC_1G_YODA_CLK_DIV_OFF);
	writel(reg, priv->clk_div_base);

	/* Re-enable clock */
	reg &= ~EMAC_1G_CG_ENABLE;
	writel(reg, priv->clk_div_base);

	return 0;
}

static int adi_clk_of_xlate(struct clk *clk, struct ofnode_phandle_args *args)
{
	if (args->args_count == 0) {
		debug("Invalid args_count: %d\n", args->args_count);
		return -EINVAL;
	}

	sam_priv.phy_interface = args->args[0]; /* speed: 10|100|1000 */

	clk->id = 1;                            /* arbitrary ID */

	return 0;
}

static ulong adi_clk_set_rate(struct clk *clk, ulong rate)
{
	return set_clk_divider(&sam_priv, rate);
}

const struct clk_ops adi_clk_ops = {
	.of_xlate	= adi_clk_of_xlate,
	.set_rate	= adi_clk_set_rate,
};

static int adrv906x_1g_clock_probe(struct udevice *dev)
{
	int ret;
	const char *clk_name;
	struct clk clkin;

	/* ADRV906X ethernet 1g input clock  */
	clk_name = dev_read_string(dev, "clock-names");
	if (clk_name == NULL) {
		pr_err("failed to get /%s/clock-names\n", dev->name);
		return -1;
	}

	ret = clk_get_by_name(dev, "dwmac_clkin", &clkin);
	if (ret < 0) {
		pr_err("failed to get %s clock\n", clk_name);
		return ret;
	}

	/* Fill clk info */
	sam_priv.clk_div_base = dev_remap_addr(dev);
	sam_priv.base_clk_speed = clk_get_rate(&clkin);

	return ret;
}

static const struct udevice_id adi_adrv906x_1g_clk_ids[] = {
	{ .compatible = "adi,adrv906x-1g-clock" },
	{ },
};

U_BOOT_DRIVER(adi_adrv906x_1g_clk) = {
	.name		= "clk_adi_adrv906x_1g",
	.id		= UCLASS_CLK,
	.of_match	= adi_adrv906x_1g_clk_ids,
	.ops		= &adi_clk_ops,
	.probe		= adrv906x_1g_clock_probe,
	.flags		= DM_FLAG_PRE_RELOC,
};
