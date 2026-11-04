// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Pin controller driver for the Alif Ensemble SoCs. The device tree
 * binding is the same as the one of Linux.
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <dm/pinctrl.h>
#include <linux/io.h>
#include <linux/libfdt.h>

/* One 32 bit register per pin, there are 15 ports of 8 pins */
#define ALIF_PINCTRL_NUM_PINS		120
#define ALIF_PINCTRL_MUX_MASK		GENMASK(3, 0)
#define ALIF_PINCTRL_PAD_MASK		GENMASK(7, 0)
#define ALIF_PINCTRL_PAD_SHIFT		16

/* The pin and the function are encoded in the first cell of a pair */
#define ALIF_PINMUX_PIN(x)		((x) & 0xffff)
#define ALIF_PINMUX_FUNC(x)		(((x) >> 16) & 0xf)

struct alif_pinctrl_priv {
	void __iomem *base;
};

static int alif_pinctrl_set_state(struct udevice *dev, struct udevice *config)
{
	struct alif_pinctrl_priv *priv = dev_get_priv(dev);
	const fdt32_t *list;
	int len, i;

	list = dev_read_prop(config, "pinmux", &len);
	if (!list)
		return -EINVAL;

	/* Each entry is a pair of the pin with its function and the pad control */
	if (len % (2 * sizeof(fdt32_t)))
		return -EINVAL;

	for (i = 0; i < len / (2 * sizeof(fdt32_t)); i++) {
		u32 pinmux = fdt32_to_cpu(list[2 * i]);
		u32 pad = fdt32_to_cpu(list[2 * i + 1]);
		u32 pin = ALIF_PINMUX_PIN(pinmux);

		if (pin >= ALIF_PINCTRL_NUM_PINS) {
			dev_err(dev, "%s: unsupported pin %u\n", config->name, pin);
			return -EINVAL;
		}

		writel((ALIF_PINMUX_FUNC(pinmux) & ALIF_PINCTRL_MUX_MASK) |
		       ((pad & ALIF_PINCTRL_PAD_MASK) << ALIF_PINCTRL_PAD_SHIFT),
		       priv->base + pin * sizeof(u32));
	}

	return 0;
}

static int alif_pinctrl_probe(struct udevice *dev)
{
	struct alif_pinctrl_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	return 0;
}

static const struct pinctrl_ops alif_pinctrl_ops = {
	.set_state = alif_pinctrl_set_state,
};

static const struct udevice_id alif_pinctrl_ids[] = {
	{ .compatible = "alif,ensemble-pinctrl" },
	{ }
};

U_BOOT_DRIVER(alif_ensemble_pinctrl) = {
	.name = "alif_ensemble_pinctrl",
	.id = UCLASS_PINCTRL,
	.of_match = alif_pinctrl_ids,
	.probe = alif_pinctrl_probe,
	.ops = &alif_pinctrl_ops,
	.priv_auto = sizeof(struct alif_pinctrl_priv),
};
