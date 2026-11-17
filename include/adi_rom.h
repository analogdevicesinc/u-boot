/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OTP functions of the 32-bit ADSP-SC5xx boot ROM API, and calling
 * them from U-Boot proper.
 *
 * The SoC provides the jump table entries (arch/arm/mach-sc5xx/sc58x.c) and
 * arch/arm/mach-sc5xx/rom.c makes the ROM executable; the sandbox build
 * replaces both with an emulated ROM (drivers/misc/adi-otp-sc58x-sandbox.c).
 *
 * Copyright 2026 Analog Devices Inc.
 */
#ifndef __ADI_ROM_H
#define __ADI_ROM_H

#include <linux/types.h>

/**
 * struct adi_rom_otp_api - the OTP functions of the boot ROM API, by their
 * jump table entries (CCES defSC58x_rom_jumptable.h, cdefSC58x_rom.h)
 * @get: bool otp_get(OTPCMD cmd, uint32_t data[]): read the field @cmd
 * @get_data: bool otp_get_data(uint32_t data[], uint16_t offset,
 *	uint16_t sizeIn32bitWords): read @words 32-bit words at byte offset
 *	@offset of the OTP
 * @pgm: bool otp_pgm(otp_data *data): program the fields of @otp_data
 *	(a SoC-specific struct otp_data of 32-bit physical addresses and
 *	values); null addresses and zero values are left alone
 * @lock: bool otp_lock(void): lock the part, from the next reset on
 * @cfg: bool otp_cfg(void): configure the OTP controller
 *
 * All return true on success. The ROM is little-endian Thumb code and
 * follows the AAPCS, so a bool comes back zero-extended in r0. Call them
 * only between adi_rom_enter() and adi_rom_exit(). Pointers are passed as
 * is: U-Boot maps memory 1:1.
 */
struct adi_rom_otp_api {
	bool (*get)(u32 cmd, u32 *data);
	bool (*get_data)(u32 *data, u16 offset, u16 words);
	bool (*pgm)(void *otp_data);
	bool (*lock)(void);
	bool (*cfg)(void);
};

/* The ADSP-SC58x ROM's OTP functions (arch/arm/mach-sc5xx/sc58x.c) */
extern const struct adi_rom_otp_api adi_sc58x_rom_otp_api;

/**
 * adi_rom_enter() - make the boot ROM executable
 *
 * U-Boot maps everything outside DRAM, the boot ROM included, as
 * execute-never. Calls may nest; the ROM stays executable until the
 * matching number of adi_rom_exit() calls.
 */
void adi_rom_enter(void);

/**
 * adi_rom_exit() - end what adi_rom_enter() started
 */
void adi_rom_exit(void);

#endif /* __ADI_ROM_H */
