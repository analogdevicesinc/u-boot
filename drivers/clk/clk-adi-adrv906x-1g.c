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
 * Note: This light driver is not based on CCF (Common Clock Framework)
 * Note: ADRV906x does not support the option for an external 50 MHz clock for
 *       the RMII PHY. This clock must be provided by the MAC.
 */

#define EMAC_1G_CG_ENABLE                       BIT(0)
#define EMAC_1G_OSC_CLK_DIV_MASK                GENMASK(19, 13)
#define EMAC_1G_CLK_DIV_MASK                    GENMASK(12, 6)
#define EMAC_1G_PHY_INTF_SEL_I_MASK             GENMASK(5, 3)
#define EMAC_1G_OSC_CLK_DIV_OFF                 13
#define EMAC_1G_CLK_DIV_OFF                     6
#define EMAC_1G_PHY_INTF_SEL_I_OFF              3
#define EMAC_1G_PHY_INTF_SEL_I_RMII             4
#define EMAC_1G_PHY_INTF_SEL_I_RGMII            1

#define ETH1G_DEVCLK_MASK                       GENMASK(13, 6)
#define ETH1G_DEVCLK_DIV_FUND                   BIT(6)
#define ETH1G_DEVCLK_DIV_KILLCLK                0       /* BIT(7) */
#define ETH1G_DEVCLK_DIV_MCS_RESET              0       /* BIT(8) */
#define ETH1G_DEVCLK_DIV_RATIO                  0       /* Bits 9-10 */
#define ETH1G_DEVCLK_DIV_RB                     BIT(11)
#define ETH1G_DEVCLK_BUFFER_ENABLE              BIT(12)
#define ETH1G_DEVCLK_BUFFER_TERM_ENABLE         BIT(13)
#define ETH1G_DEVCLK_DEFAULT_VAL                (ETH1G_DEVCLK_DIV_FUND |       \
						 ETH1G_DEVCLK_DIV_KILLCLK |    \
						 ETH1G_DEVCLK_DIV_MCS_RESET |  \
						 ETH1G_DEVCLK_DIV_RATIO |      \
						 ETH1G_DEVCLK_DIV_RB |         \
						 ETH1G_DEVCLK_BUFFER_ENABLE)

#define ETH1G_REFCLK_MASK                       BIT(17)
#define ETH1G_REFCLK_REFPATH_PD                 0 /* BIT(17) */
#define ETH1G_REFCLK_DEFAULT_VAL                ETH1G_REFCLK_REFPATH_PD

#define HZ_TO_MHZ(freq)                         (freq * 1000 * 1000)
#define CLK_25MHZ                               HZ_TO_MHZ(25)
#define CLK_50MHZ                               HZ_TO_MHZ(50)
#define CLK_125MHZ                              HZ_TO_MHZ(125)

struct adrv906x_priv_data {
	uint32_t base_clk_speed;
	uint32_t phy_interface;
	void __iomem *clk_div_base;
};

struct adrv906x_priv_data sam_priv;

static int adrv906x_dwmac_set_clk_dividers(struct adrv906x_priv_data *priv, ulong rate)
{
	uint32_t reg;
	uint32_t osc_div;
	uint32_t rmii_div;

	/* Sanity checks */
	if ((priv->base_clk_speed % rate) != 0) {
		pr_err("Unable to get this clock rate (%ld Hz)\n", rate);
		return -EINVAL;
	}

	if ((priv->phy_interface == PHY_INTERFACE_MODE_RMII) &&
	    ((priv->base_clk_speed % CLK_50MHZ) != 0)) {
		pr_err("Unable to get RMII PHY clock (50 MHz)\n");
		return -1;
	}


	if ((priv->phy_interface == PHY_INTERFACE_MODE_RMII) &&
	    (rate == CLK_125MHZ)) {
		pr_err("RMII does not support 1000 Mbs");
		return -1;
	}

	if ((priv->phy_interface != PHY_INTERFACE_MODE_RMII) &&
	    (priv->phy_interface != PHY_INTERFACE_MODE_RGMII)) {
		pr_err("MAC-PHY Interface (%d) not supported", priv->phy_interface);
		return -1;
	}

	/* Compute clock dividers */
	if (priv->phy_interface == PHY_INTERFACE_MODE_RMII) {
		/* input_clk   _|-> OSC_CLK_DIV (50 MHz) ------> PHY core clock (REF_CLK)
		 * (ie 250MHz)  |-> RMII_CLK_DIV (2.5|25 MHz) -> Tx_clk and Rx_clk to GMAC
		 *                  (based on PHY link 10|100)
		 */
		osc_div = (priv->base_clk_speed / CLK_50MHZ) - 1;
		rmii_div = ((priv->base_clk_speed) / (rate)) - 1;
	} else if (priv->phy_interface == PHY_INTERFACE_MODE_RGMII) {
		/*
		 * input_clk     -> OSC_CLK_DIV (2.5|25|125 MHz) -> Tx_clk to PHY and Tx_clk to GMAC
		 * (ie 250MHz)      (based on PHY link 10|100|1000)
		 *
		 * Note: Rx_clk to GMAC IP is provided by the external PHY
		 * Note: RMII_CLK_DIV does not apply
		 * Note: PHY core clock is provided by a crystal circuitry
		 */
		osc_div = ((priv->base_clk_speed) / rate) - 1;
		rmii_div = 0;
	}

	/* Disable clock */
	reg = readl(priv->clk_div_base);
	reg |= EMAC_1G_CG_ENABLE;
	writel(reg, priv->clk_div_base);

	/* Set clock divider */
	if (priv->phy_interface == PHY_INTERFACE_MODE_RMII) {
		reg &= ~EMAC_1G_PHY_INTF_SEL_I_MASK;
		reg |= EMAC_1G_PHY_INTF_SEL_I_RMII << EMAC_1G_PHY_INTF_SEL_I_OFF;
	} else if (priv->phy_interface == PHY_INTERFACE_MODE_RGMII) {
		reg &= ~EMAC_1G_PHY_INTF_SEL_I_MASK;
		reg |= EMAC_1G_PHY_INTF_SEL_I_RGMII << EMAC_1G_PHY_INTF_SEL_I_OFF;
	}
	reg &= ~(EMAC_1G_OSC_CLK_DIV_MASK | EMAC_1G_CLK_DIV_MASK);
	reg |= (osc_div << EMAC_1G_OSC_CLK_DIV_OFF) |
	       (rmii_div << EMAC_1G_CLK_DIV_OFF);
	writel(reg, priv->clk_div_base);

	/* Re-enable clock */
	reg &= ~EMAC_1G_CG_ENABLE;
	writel(reg, priv->clk_div_base);

	return 0;
}

static int adi_clk_enable(struct clk *clk)
{
	/* Configure clock distribution (depends on the phy interface type).
	 * Enable Link-speed-related clocks to arbitrary value
	 * Enable PHY core clock to 50 MHz (only for RMII)
	 */
	return adrv906x_dwmac_set_clk_dividers(&sam_priv, CLK_25MHZ);
}

static int adi_clk_of_xlate(struct clk *clk, struct ofnode_phandle_args *args)
{
	if (args->args_count == 0) {
		debug("Invalid args_count: %d\n", args->args_count);
		return -EINVAL;
	}

	sam_priv.phy_interface = args->args[0]; /* Phy interface */

	clk->id = 1;                            /* arbitrary ID */

	return 0;
}

static ulong adi_clk_set_rate(struct clk *clk, ulong rate)
{
	return (ulong)adrv906x_dwmac_set_clk_dividers(&sam_priv, rate);
}

const struct clk_ops adi_clk_ops = {
	.of_xlate	= adi_clk_of_xlate,
	.set_rate	= adi_clk_set_rate,
	.enable		= adi_clk_enable,
};

static void adrv906x_clk_buffer_enable(void __iomem *clk_ctrl_base, bool term_en)
{
	uint32_t val;

	clk_ctrl_base = (void __iomem *)0x20190000;
	/* eth1_devclk: enable buffer and divide by 1 (buffered input clock) */
	val = readl(clk_ctrl_base);
	val &= ~ETH1G_DEVCLK_MASK;
	val |= ETH1G_DEVCLK_DEFAULT_VAL;
	if (term_en)
		/* Enable internal terminator resistor */
		val |= ETH1G_DEVCLK_BUFFER_TERM_ENABLE;
	writel(val, clk_ctrl_base);

	/* eth1_refclk: enable clock long-route driver */
	val = readl(clk_ctrl_base + 0x04);
	val &= ~ETH1G_REFCLK_MASK;
	val |= ETH1G_REFCLK_DEFAULT_VAL;
	iowrite32(val, clk_ctrl_base + 0x04);
}

static int adrv906x_1g_clock_probe(struct udevice *dev)
{
	int ret;
	const char *clk_name;
	struct clk clkin;
	void __iomem *clk_ctrl_base;
	bool term_en;
	uint32_t adi_ctrl_reg[2];

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

	/* ADRV906x ethernet 1g Buffer input clock */
	ret = dev_read_u32_array(dev, "adi,ctrl-reg", adi_ctrl_reg, sizeof(adi_ctrl_reg) / sizeof(adi_ctrl_reg[0]));
	if (ret < 0) {
		pr_err("failed to get clock control base address\n");
		return ret;
	}
	clk_ctrl_base = map_physmem(adi_ctrl_reg[0], adi_ctrl_reg[1], MAP_NOCACHE);
	term_en = dev_read_bool(dev, "adi,term_en");

	/* Enable input clock buffer */
	adrv906x_clk_buffer_enable(clk_ctrl_base, term_en);

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
