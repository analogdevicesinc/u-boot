// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 * Converted to driver model by Nathan Barrett-Morrison
 *
 * Author: Greg Malysa <greg.malysa@timesys.com>
 *
 * dm timer implementation for ADI ADSP-SC5xx SoCs
 *
 */

#include <clk.h>
#include <dm.h>
#include <timer.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>

/* Shared group registers (relative to block base) */
#define GPTIMER_RUN_SET		0x08
#define GPTIMER_DATA_IMSK	0x1C

/* Per-timer registers (relative to timer base = block base + adi,offset) */
#define GPTIMER_CFG_OFF		0x00
#define GPTIMER_CNT_OFF		0x04
#define GPTIMER_PER_OFF		0x08
#define GPTIMER_WID_OFF		0x0C

/* Timer Configuration Register bits */
#define TIMER_OUT_DIS		0x0800
#define TIMER_PULSE_HI		0x0080
#define TIMER_MODE_PWM_CONT	0x000c

#define MAX_TIM_LOAD		0xFFFFFFFF

struct adi_gptimer_priv {
	void __iomem *group_base;
	void __iomem *timer_base;
	u32 prev;
	u64 upper;
};

static u64 adi_gptimer_get_count(struct udevice *udev)
{
	struct adi_gptimer_priv *priv = dev_get_priv(udev);

	u32 now = readl(priv->timer_base + GPTIMER_CNT_OFF);

	if (now < priv->prev)
		priv->upper += (1ull << 32);

	priv->prev = now;

	return priv->upper + (u64)now;
}

static const struct timer_ops adi_gptimer_ops = {
	.get_count = adi_gptimer_get_count,
};

static int adi_gptimer_probe(struct udevice *udev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(udev);
	struct adi_gptimer_priv *priv = dev_get_priv(udev);
	void __iomem *group_base;
	ofnode child;
	struct clk clk;
	u32 id, offset;
	u16 imask;
	int ret;
	bool found = false;

	group_base = dev_remap_addr(udev);
	if (!group_base)
		return -EINVAL;

	ofnode_for_each_subnode(child, dev_ofnode(udev)) {
		if (!ofnode_read_bool(child, "adi,is-clocksource"))
			continue;
		if (ofnode_read_u32(child, "reg", &id) ||
		    ofnode_read_u32(child, "adi,offset", &offset)) {
			dev_err(udev, "clocksource child missing reg or adi,offset\n");
			return -EINVAL;
		}
		found = true;
		break;
	}

	if (!found) {
		dev_err(udev, "no child with adi,is-clocksource found\n");
		return -ENODEV;
	}

	priv->group_base = group_base;
	priv->timer_base = group_base + offset;
	priv->upper = 0;
	priv->prev = 0;

	ret = clk_get_by_index(udev, 0, &clk);
	if (ret < 0) {
		dev_err(udev, "missing clock reference for timer\n");
		return ret;
	}

	ret = clk_enable(&clk);
	if (ret) {
		dev_err(udev, "failed to enable clock\n");
		return ret;
	}

	uc_priv->clock_rate = clk_get_rate(&clk);

	writew(TIMER_OUT_DIS | TIMER_MODE_PWM_CONT | TIMER_PULSE_HI,
	       priv->timer_base + GPTIMER_CFG_OFF);
	writel(MAX_TIM_LOAD, priv->timer_base + GPTIMER_PER_OFF);
	writel(MAX_TIM_LOAD - 1, priv->timer_base + GPTIMER_WID_OFF);

	imask = readw(group_base + GPTIMER_DATA_IMSK);
	imask &= ~(1 << id);
	writew(imask, group_base + GPTIMER_DATA_IMSK);
	writew(1 << id, group_base + GPTIMER_RUN_SET);

	return 0;
}

static const struct udevice_id adi_gptimer_ids[] = {
	{ .compatible = "adi,sc5xx-gptimers" },
	{ },
};

U_BOOT_DRIVER(adi_gptimer) = {
	.name		= "adi_gptimer",
	.id		= UCLASS_TIMER,
	.of_match	= adi_gptimer_ids,
	.priv_auto	= sizeof(struct adi_gptimer_priv),
	.probe		= adi_gptimer_probe,
	.ops		= &adi_gptimer_ops,
};
