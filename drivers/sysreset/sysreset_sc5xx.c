// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <dm.h>
#include <sysreset.h>
#include <asm/io.h>

struct sc5xx_sysreset_priv {
	void __iomem *rcu;
};

static int sc5xx_sysreset_request(struct udevice *dev, enum sysreset_t type)
{
	struct sc5xx_sysreset_priv *priv = dev_get_priv(dev);

	writel(readl(priv->rcu) | 1, priv->rcu);
	return -EINPROGRESS;
}

static int sc5xx_sysreset_probe(struct udevice *dev)
{
	struct sc5xx_sysreset_priv *priv = dev_get_priv(dev);

	priv->rcu = dev_remap_addr(dev);
	if (!priv->rcu)
		return -EINVAL;
	return 0;
}

static const struct udevice_id sc5xx_sysreset_ids[] = {
	{ .compatible = "adi,reset-controller" },
	{ }
};

static struct sysreset_ops sc5xx_sysreset_ops = {
	.request = sc5xx_sysreset_request,
};

U_BOOT_DRIVER(sysreset_sc5xx) = {
	.name		= "sc5xx_sysreset",
	.id		= UCLASS_SYSRESET,
	.of_match	= sc5xx_sysreset_ids,
	.ops		= &sc5xx_sysreset_ops,
	.probe		= sc5xx_sysreset_probe,
	.priv_auto	= sizeof(struct sc5xx_sysreset_priv),
};
