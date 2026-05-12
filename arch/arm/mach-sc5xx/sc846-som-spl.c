// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <asm/arch/spl.h>

// Boot arguments for SC846 SoC
const struct adi_boot_args adi_rom_boot_args[] = {
	// JTAG/no boot
	[0] = {0, 0, 0},
	// SPI master, used for qspi as well
	[1] = {0x70040000, 0x00040000, 0x00010107},
	// SPI slave
	[2] = {0, 0, 0x00000212},
	// UART slave
	[3] = {0, 0, 0x00000013},
	// Linkport slave
	[4] = {0, 0, 0x00000014},
	// XSPI master
	[5] = {0x60040000, 0, 0x0000000A},
	// reserved, also no boot
	[7] = {0, 0, 0}
};
