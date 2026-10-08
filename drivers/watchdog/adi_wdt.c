// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 * Converted to driver model by Nathan Barrett-Morrison
 *
 *
 * adi_wtd.c - driver for ADI on-chip watchdog
 *
 */

#include <clk.h>
#include <dm.h>
#include <regmap.h>
#include <syscon.h>
#include <wdt.h>
#include <linux/delay.h>

#define WDOG_CTL  0x0
#define WDOG_CNT  0x4
#define WDOG_STAT 0x8

#define RCU_CTL   0x0
#define RCU_STAT  0x4

#define SEC_GCTL  0x0
#define SEC_FCTL  0x10
#define SEC_SCTL0 0x800

#define WDEN      0x0010
#define WDDIS     0x0AD0

struct adi_wdt_priv {
	struct regmap *rcu;
	struct regmap *sec;
	struct regmap *wdt;
	u32 secid;
	struct clk clock;
};

static int adi_wdt_reset(struct udevice *dev)
{
	struct adi_wdt_priv *priv = dev_get_priv(dev);

	regmap_write(priv->wdt, WDOG_STAT, 0);

	return 0;
}

static int adi_wdt_start(struct udevice *dev, u64 timeout_ms, ulong flags)
{
	struct adi_wdt_priv *priv = dev_get_priv(dev);
	u32 val;

	/* Disable SYSCD_RESETb input and clear the RCU0 reset status */
	regmap_write(priv->rcu, RCU_STAT, 0xf);
	regmap_write(priv->rcu, RCU_CTL, 0x0);

	/* reset the SEC controller */
	regmap_write(priv->sec, SEC_GCTL, 0x2);
	regmap_write(priv->sec, SEC_FCTL, 0x2);

	udelay(50);

	/* enable SEC fault event */
	regmap_write(priv->sec, SEC_GCTL, 0x1);

	/* ANOMALY 36100004 Spurious External Fault event occurs when FCTL
	 * is re-programmed when currently active fault is not cleared
	 */
	regmap_write(priv->sec, SEC_FCTL, 0xc0);
	regmap_write(priv->sec, SEC_FCTL, 0xc1);

	/* enable SEC fault source for watchdog0 */
	regmap_read(priv->sec, SEC_SCTL0 + (priv->secid * 8), &val);
	regmap_write(priv->sec, SEC_SCTL0 + (priv->secid * 8), val | 0x6);

	/* Enable SYSCD_RESETb input */
	regmap_write(priv->rcu, RCU_CTL, 0x100);

	/* enable watchdog0 */
	regmap_write(priv->wdt, WDOG_CTL, WDDIS);

	regmap_write(priv->wdt, WDOG_CNT,
		     timeout_ms / 1000 *
		     (clk_get_rate(&priv->clock) / (IS_ENABLED(CONFIG_SC58X) ? 2 : 1)));

	regmap_write(priv->wdt, WDOG_STAT, 0);
	regmap_write(priv->wdt, WDOG_CTL, WDEN);

	return 0;
}

static int adi_wdt_probe(struct udevice *dev)
{
	struct adi_wdt_priv *priv = dev_get_priv(dev);
	int ret;
	u32 secid;

	priv->rcu = syscon_regmap_lookup_by_phandle(dev, "adi,rcu");
	if (IS_ERR(priv->rcu))
		return PTR_ERR(priv->rcu);

	priv->sec = syscon_regmap_lookup_by_phandle(dev, "adi,sec");
	if (IS_ERR(priv->sec))
		return PTR_ERR(priv->sec);

	ret = regmap_init_mem(dev_ofnode(dev), &priv->wdt);
	if (ret)
		return ret;

	ret = dev_read_u32(dev, "secid", &secid);
	if (ret)
		return ret;
	priv->secid = secid;

	ret = clk_get_by_name(dev, "sclk0", &priv->clock);
	if (ret < 0) {
		printf("Can't get WDT clk: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct wdt_ops adi_wdt_ops = {
	.start		= adi_wdt_start,
	.reset		= adi_wdt_reset,
};

static const struct udevice_id adi_wdt_ids[] = {
	{ .compatible = "adi,watchdog" },
	{}
};

U_BOOT_DRIVER(adi_wdt) = {
	.name		= "adi_wdt",
	.id		= UCLASS_WDT,
	.of_match	= adi_wdt_ids,
	.probe		= adi_wdt_probe,
	.ops		= &adi_wdt_ops,
	.priv_auto = sizeof(struct adi_wdt_priv),
	.flags		= DM_FLAG_PRE_RELOC,
};
