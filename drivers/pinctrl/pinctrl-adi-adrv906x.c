/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Author: Howard Massey <Howard.Massey@analog.com>
 *
 * pinctrl implementation for ADI ADRV906X SoC
 *
 */

#include <common.h>
#include <dm.h>
#include <dm/device.h>
#include <dm/pinctrl.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/io.h>

#define ADI_PINCTRL_SIP_SERVICE_FUNCTION_ID                     0xC2000001

/* ADI Pinctrl SIP Service Functions*/
#define ADI_PINCTRL_INIT (0U)
#define ADI_PINCTRL_SET  (1U)
#define ADI_PINCTRL_GET  (2U)

/* SMC Handler return Status Values (res.a0 return value) */
#define ADI_PINCTRL_SMC_RETURN_SUCCESS                  (0U)
#define ADI_PINCTRL_SMC_RETURN_UNSUPPORTED_REQUEST      (0xFFFFFFFFFFFFFFFFU)

/* SMC Pinctrl Handler return values (res.a1 return value) */
#define ADI_TFA_PINCTRL_HANDLER_FAILURE                 (0U)
#define ADI_TFA_PINCTRL_HANDLER_SUCCESS                 (1U)

/* SMC Config Bitfield Config Word */
#define ADI_BITFIELD_ST_BIT_POSITION                    (0U)
#define ADI_BITFIELD_PULL_ENABLEMENT_BIT_POSITION       (1U)
#define ADI_BITFIELD_PULLUP_ENABLE_BIT_POSITION         (2U)

/* SMC GET result defines*/
#define ADI_GET_BITFIELD_1_PIN_CONFIGURED_BIT_POSITION (63U)

/*
 * Bit Mask Info for ADI's Pinctrl Word
 */
#define ADI_CONFIG_DRIVE_STRENGTH_MASK                        (0x0000000FU)
#define ADI_CONFIG_DRIVE_STRENGTH_MASK_BIT_POSITION           (0U)
#define ADI_CONFIG_SCHMITT_TRIG_ENABLE_MASK                   (0x00000010U)
#define ADI_CONFIG_SCHMITT_TRIG_ENABLE_MASK_BIT_POSITION      (4U)
#define ADI_CONFIG_PULL_UP_DOWN_ENABLEMENT_MASK               (0x00000020U)
#define ADI_CONFIG_PULL_UP_DOWN_ENABLEMENT_MASK_BIT_POSITION  (5U)
#define ADI_CONFIG_PULLUP_ENABLE_MASK                         (0x00000040U)
#define ADI_CONFIG_PULLUP_ENABLE_MASK_BIT_POSITION            (6)

struct adi_adrv906x_pinctrl_priv {
	ulong base;
	int n_pins;
	int number_dedicated_io;
	int dedicated_io_start_pin_num;
};

static int adi_adrv906x_pinctrl_pinmux_set(struct udevice *udev, unsigned pin, unsigned src_mux, unsigned config)
{
	struct arm_smccc_res res;
	int drive_strength;
	int schmitt_trig_enable;
	int pin_pull_enablement;
	int pin_pull_up_enable;
	int config_bitfield;
	struct adi_adrv906x_pinctrl_priv *priv = dev_get_priv(udev);
	bool pin_is_dio = false;

	if (pin >= priv->dedicated_io_start_pin_num && pin < (priv->dedicated_io_start_pin_num + priv->number_dedicated_io))
		pin_is_dio = true;

	if (pin >= priv->n_pins && !pin_is_dio)
		return -ENODEV;

	/*
	 * Setup  smc call to perform the pinconf_set operation
	 *
	 * arm_smccc_smc expected params:
	 *    param1: SMC SIP SERVICE ID
	 *    param2: ADI Pinctrl request (GET, SET, INIT)
	 *    param3: Pin Number requested
	 *    param4: Source Mux setting requested
	 *    param5: Drive Strength
	 *    param6: BIT_FIELD-3bits-(SchmittTrigEnable | PU PD Enablement | PU Enable)
	 *    param7: Base Address
	 *    param8: Currently UNUSED/UNDEFINED
	 *    param9: response output of the SMC call
	 *               a0=SMC return value
	 *               a1=ADI TFA Pinctrl Handler return status
	 *               a2=ADI unused
	 *               a3=ADI unused
	 *
	 */

	drive_strength = config & ADI_CONFIG_DRIVE_STRENGTH_MASK;
	schmitt_trig_enable = (config & ADI_CONFIG_SCHMITT_TRIG_ENABLE_MASK) ? 1 : 0;
	pin_pull_enablement = (config & ADI_CONFIG_PULL_UP_DOWN_ENABLEMENT_MASK) ? 1 : 0;
	pin_pull_up_enable = (config & ADI_CONFIG_PULLUP_ENABLE_MASK) ? 1 : 0;
	config_bitfield = (schmitt_trig_enable << ADI_BITFIELD_ST_BIT_POSITION) |
			  (pin_pull_enablement << ADI_BITFIELD_PULL_ENABLEMENT_BIT_POSITION) |
			  (pin_pull_up_enable << ADI_BITFIELD_PULLUP_ENABLE_BIT_POSITION);

	arm_smccc_smc(ADI_PINCTRL_SIP_SERVICE_FUNCTION_ID,
		      ADI_PINCTRL_SET,
		      pin,
		      src_mux,
		      drive_strength,
		      config_bitfield,
		      priv->base,
		      0,
		      &res);

	/*
	 *  The SMC call return status is present in res.a0,
	 *     the pinctrl TFA Handler is present in res.a1
	 */
	if (res.a0 != ADI_PINCTRL_SMC_RETURN_SUCCESS || res.a1 != ADI_TFA_PINCTRL_HANDLER_SUCCESS)
		return -EINVAL;

	return 0;
}

static int adi_adrv906x_pinctrl_set_state(struct udevice *udev, struct udevice *config)
{
	const struct fdt_property *pinlist;
	int length = 0;
	int ret, i;
	u32 pin, src_mux, config_word;

	pinlist = dev_read_prop(config, "adi,pins", &length);
	if (!pinlist)
		return -EINVAL;

	if (length % (sizeof(uint32_t) * 3))
		return -EINVAL;

	for (i = 0; i < length / sizeof(uint32_t); i += 3) {
		ret = dev_read_u32_index(config, "adi,pins", i, &pin);
		if (ret)
			return ret;

		ret = dev_read_u32_index(config, "adi,pins", i + 1, &src_mux);
		if (ret)
			return ret;

		ret = dev_read_u32_index(config, "adi,pins", i + 2, &config_word);
		if (ret)
			return ret;

		ret = adi_adrv906x_pinctrl_pinmux_set(udev, pin, src_mux, config_word);
		if (ret)
			return ret;
	}

	return 0;
}

const struct pinctrl_ops adi_adrv906x_pinctrl_ops = {
	.set_state	= adi_adrv906x_pinctrl_set_state,
};

static int adi_adrv906x_pinctrl_probe(struct udevice *udev)
{
	struct adi_adrv906x_pinctrl_priv *priv = dev_get_priv(udev);

	priv->base = dev_read_addr(udev);
	if (priv->base == FDT_ADDR_T_NONE)
		return -EINVAL;

	if (dev_read_u32_index(udev, "adi,pin-info", 0, &priv->n_pins))
		return -ENOENT;

	if (dev_read_u32_index(udev, "adi,pin-info", 1, &priv->number_dedicated_io))
		return -ENOENT;

	if (dev_read_u32_index(udev, "adi,pin-info", 2, &priv->dedicated_io_start_pin_num))
		return -ENOENT;

	if (!priv->n_pins || !priv->dedicated_io_start_pin_num || !priv->number_dedicated_io)
		return -ENOENT;

	return 0;
}

static const struct udevice_id adi_adrv906x_pinctrl_match[] = {
	{ .compatible = "adi,adrv906x-pinctrl" },
	{ },
};

U_BOOT_DRIVER(adi_adrv906x_pinctrl) = {
	.name		= "adi_adrv906x_pinctrl",
	.id		= UCLASS_PINCTRL,
	.of_match	= adi_adrv906x_pinctrl_match,
	.probe		= adi_adrv906x_pinctrl_probe,
	.priv_auto	= sizeof(struct adi_adrv906x_pinctrl_priv),
	.ops		= &adi_adrv906x_pinctrl_ops,
	.flags		= DM_FLAG_PRE_RELOC,
};
