/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 *
 */

#ifndef DT_BINDINGS_PINCTRL_ADI_ADSP
#define DT_BINDINGS_PINCTRL_ADI_ADSP

#define ADI_ADSP_PINFUNC_GPIO 0
#define ADI_ADSP_PINFUNC_ALT0 1
#define ADI_ADSP_PINFUNC_ALT1 2
#define ADI_ADSP_PINFUNC_ALT2 3
#define ADI_ADSP_PINFUNC_ALT3 4

#define ADI_ADSP_PINMUX(port, pin, func) \
	((((port) - 'A') * 16 + (pin)) << 8 | (func))

#endif
