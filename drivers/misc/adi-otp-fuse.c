// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ADI ADSP OTP driver: the "fuse" command API.
 *
 * Bank n is the n-th ADI OTP device (bank 0 on parts with one OTP), and a
 * word is a 32-bit word of its OTP area: word = byte offset / 4, with the
 * offsets of the SoC's OTP layout (doc/board/adi/adsp-otp.rst). Reserved
 * words read as 0. Programming, by "fuse prog" or "fuse writebuff",
 * follows the rules of the writebuff buffer; there are no shadow
 * registers, so "fuse sense" reads the OTP like "fuse read", and "fuse
 * override" is not supported.
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <dm.h>
#include <errno.h>
#include <fuse.h>
#include <mapmem.h>
#include <asm/byteorder.h>

#include "adi-otp.h"

int fuse_read(u32 bank, u32 word, u32 *val)
{
	struct udevice *dev;
	int ret;

	ret = adi_otp_get_device(bank, &dev);
	if (ret)
		return ret;

	return adi_otp_read_word(dev, word, val);
}

int fuse_sense(u32 bank, u32 word, u32 *val)
{
	return fuse_read(bank, word, val);
}

int fuse_prog(u32 bank, u32 word, u32 val)
{
	struct udevice *dev;
	int ret;

	ret = adi_otp_get_device(bank, &dev);
	if (ret)
		return ret;

	return adi_otp_program_word(dev, word, val);
}

int fuse_override(u32 bank, u32 word, u32 val)
{
	return -EPERM;
}

#ifdef CONFIG_CMD_FUSE_WRITEBUFF
/* The buffer names its SoC; it is handed to the first ADI OTP device */
int fuse_writebuff(ulong addr)
{
	const struct adi_otp_buf_hdr *hdr;
	struct udevice *dev;
	const void *buf;
	u32 size;
	int ret;

	ret = adi_otp_get_device(0, &dev);
	if (ret)
		return ret;

	/* Map as much as the header says; the driver checks the header */
	hdr = map_sysmem(addr, sizeof(*hdr));
	size = le32_to_cpu(hdr->total_size);
	unmap_sysmem(hdr);
	if (size < sizeof(*hdr) || size > ADI_OTP_BUF_MAX_SIZE)
		size = sizeof(*hdr);

	buf = map_sysmem(addr, size);
	ret = adi_otp_write_buffer(dev, buf);
	unmap_sysmem(buf);

	return ret;
}
#endif
