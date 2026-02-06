/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2023 - Analog Devices, Inc.
 *
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <fdtdec.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/gpio.h>

#include "adi_adrv906x_gpio.h"

/* Calculate the GPIO register and bit offset based on the pin number (0-115) */
#define GET_GPIO_REG(pin)       (pin / 32)
#define GET_GPIO_OFFSET(pin)    (pin % 32)

#define GPIO_DIR_CONTROL_SIZE    (4)    /* Size of each GPIO mode direction control register */
#define GPIO_REG_NUM    (4)             /* Number of GPIO registers */
#define GPIO_FUNCTION_NUM       (5)     /* Number of GPIO functions (write, clear, set, toggle, read) */

#define GPIO_LINE_DIRECTION_IN   1
#define GPIO_LINE_DIRECTION_OUT  0

#define GPIO_PIN_STATE_LOW      0
#define GPIO_PIN_STATE_HIGH     1

static ulong adrv906x_reg_base_s[GPIO_REG_NUM][GPIO_FUNCTION_NUM] = {
	{ GPIO_WRITE_REG0_OFFSET, GPIO_WRITE_REG0_CLEAR_OFFSET, GPIO_WRITE_REG0_SET_OFFSET,
	  GPIO_WRITE_REG0_TOGGLE_OFFSET, GPIO_READ_REG0_OFFSET },
	{ GPIO_WRITE_REG1_OFFSET, GPIO_WRITE_REG1_CLEAR_OFFSET, GPIO_WRITE_REG1_SET_OFFSET,
	  GPIO_WRITE_REG1_TOGGLE_OFFSET, GPIO_READ_REG1_OFFSET },
	{ GPIO_WRITE_REG2_OFFSET, GPIO_WRITE_REG2_CLEAR_OFFSET, GPIO_WRITE_REG2_SET_OFFSET,
	  GPIO_WRITE_REG2_TOGGLE_OFFSET, GPIO_READ_REG2_OFFSET },
	{ GPIO_WRITE_REG3_OFFSET, GPIO_WRITE_REG3_CLEAR_OFFSET, GPIO_WRITE_REG3_SET_OFFSET,
	  GPIO_WRITE_REG3_TOGGLE_OFFSET, GPIO_READ_REG3_OFFSET }
};

/* This enum must match the order of actions in adrv906x_reg_base_s */
typedef enum {
	GPIO_WRITE	= 0,
	GPIO_CLEAR	= 1,
	GPIO_SET	= 2,
	GPIO_TOGGLE	= 3,
	GPIO_READ	= 4
} gpio_mode_action_t;

struct adi_adrv906x_gpio_regs {
	u32 data;                       /* Data register */
	u32 direction;                  /* Direction register */
};

struct adi_adrv906x_gpio_plat {
	struct adi_adrv906x_gpio_regs *regs;
	int gpio_count;
	ulong base_addr;
};

static int adi_adrv906x_gpio_get_direction(struct udevice *dev, unsigned gpio)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	uint64_t gpio_mode_base_addr = plat->base_addr;
	uint32_t offset, data;
	uint64_t base_addr;

	if (gpio > plat->gpio_count)
		return -EINVAL;

	base_addr = gpio_mode_base_addr + GPIO_DIR_CONTROL_OFFSET;
	offset = gpio * GPIO_DIR_CONTROL_SIZE;

	data = readl(base_addr + offset) & GPIO_DIR_SEL_MASK;

	if (data == 0x1)
		return GPIO_LINE_DIRECTION_OUT;
	else if (data == 0x2)
		return GPIO_LINE_DIRECTION_IN;
	else
		return GPIO_LINE_DIRECTION_IN;
}

static int adi_adrv906x_gpio_direction_input(struct udevice *dev, unsigned gpio)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	uint64_t gpio_mode_base_addr = plat->base_addr;
	ulong base_addr;
	uint32_t offset, data, cleared_data;

	if (gpio > plat->gpio_count)
		return -EINVAL;

	base_addr = gpio_mode_base_addr + GPIO_DIR_CONTROL_OFFSET;
	offset = gpio * GPIO_DIR_CONTROL_SIZE;

	data = readl(base_addr + offset);
	cleared_data = data & ~GPIO_DIR_SEL_MASK;        /* clear OE and IE bits */

	writel(cleared_data | (0x1U << (GPIO_DIR_SEL_POS + 1)), base_addr + offset);

	return 0;
}

static int adi_adrv906x_gpio_set_value(struct udevice *dev, unsigned gpio, int val)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	uint32_t gpio_mode_base_addr = plat->base_addr;
	ulong base_addr;
	uint32_t offset, bitmask, reg;

	if (gpio > plat->gpio_count)
		return -EINVAL;

	offset = GET_GPIO_OFFSET(gpio);
	bitmask = 0x1U << offset;

	if (val)
		base_addr = gpio_mode_base_addr + adrv906x_reg_base_s[GET_GPIO_REG(gpio)][GPIO_SET];
	else
		base_addr = gpio_mode_base_addr + adrv906x_reg_base_s[GET_GPIO_REG(gpio)][GPIO_CLEAR];

	reg = readl(base_addr);
	reg |= bitmask;
	writel(reg, base_addr);

	return 0;
}

static int adi_adrv906x_gpio_direction_output(struct udevice *dev, unsigned gpio, int val)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	uint64_t gpio_mode_base_addr = plat->base_addr;
	ulong base_addr;
	uint32_t offset, data, cleared_data;

	if (gpio > plat->gpio_count)
		return -EINVAL;

	base_addr = gpio_mode_base_addr + GPIO_DIR_CONTROL_OFFSET;
	offset = gpio * GPIO_DIR_CONTROL_SIZE;
	data = readl(base_addr + offset);
	cleared_data = data & ~GPIO_DIR_SEL_MASK;        /* clear OE and IE bits */
	writel(cleared_data | (0x1U << GPIO_DIR_SEL_POS), base_addr + offset);

	return 0;
}

static int adi_adrv906x_gpio_get_value(struct udevice *dev, unsigned gpio)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	uint64_t gpio_mode_base_addr = plat->base_addr;
	ulong base_addr;
	uint32_t offset, data, bitmask;

	if (gpio > plat->gpio_count)
		return -EINVAL;

	base_addr = gpio_mode_base_addr + adrv906x_reg_base_s[GET_GPIO_REG(gpio)][GPIO_READ];
	offset = GET_GPIO_OFFSET(gpio);
	bitmask = 0x1U << offset;

	data = readl(base_addr) & bitmask;

	if (data)
		return GPIO_PIN_STATE_HIGH;

	return GPIO_PIN_STATE_LOW;
}

static int adi_adrv906x_gpio_get_function(struct udevice *dev, unsigned gpio)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	uint32_t reg = 0U;

	if (gpio > plat->gpio_count)
		return -EINVAL;

	/* if gpio is input, read value from the read register, else write register */
	reg = adi_adrv906x_gpio_get_direction(dev, gpio);

	if (reg == GPIO_LINE_DIRECTION_IN)
		return GPIOF_INPUT;
	else if (reg == GPIO_LINE_DIRECTION_OUT)
		return GPIOF_OUTPUT;

	return GPIOF_UNUSED;
}

static int adi_adrv906x_gpio_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);
	ulong base_addr = 0U;

	base_addr = dev_read_addr(dev);

	if (base_addr == FDT_ADDR_T_NONE) {
		debug("%s: Failed to find address\n", __func__);
		return -EINVAL;
	}

	plat->base_addr = base_addr;
	uc_priv->gpio_count = plat->gpio_count;
	uc_priv->gpio_base = 0U;

	return 0;
}

static int adi_adrv906x_gpio_of_to_plat(struct udevice *dev)
{
	struct adi_adrv906x_gpio_plat *plat = dev_get_plat(dev);

	plat->base_addr = dev_read_addr(dev);
	plat->gpio_count = fdtdec_get_int(gd->fdt_blob, dev_of_offset(dev),
					  "ngpios", 32);

	return 0;
}

static const struct dm_gpio_ops adi_adrv906x_gpio_ops = {
	.direction_input	= adi_adrv906x_gpio_direction_input,
	.direction_output	= adi_adrv906x_gpio_direction_output,
	.get_value		= adi_adrv906x_gpio_get_value,
	.set_value		= adi_adrv906x_gpio_set_value,
	.get_function		= adi_adrv906x_gpio_get_function,
};

static const struct udevice_id adi_adrv906x_gpio_ids[] = {
	{ .compatible = "adi,adrv906x-gpio" },
	{ }
};

U_BOOT_DRIVER(adi_adrv906x_gpio) = {
	.name		= "adi_adrv906x_gpio",
	.id		= UCLASS_GPIO,
	.of_match	= adi_adrv906x_gpio_ids,
	.ops		= &adi_adrv906x_gpio_ops,
	.of_to_plat	= adi_adrv906x_gpio_of_to_plat,
	.plat_auto	= sizeof(struct adi_adrv906x_gpio_plat),
	.probe		= adi_adrv906x_gpio_probe,
};
