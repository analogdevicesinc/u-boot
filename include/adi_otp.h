/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADI ADSP OTP: the "fuse writebuff" buffer format and the driver's
 * public interface. See doc/board/adi/adsp-otp.rst.
 *
 * Copyright 2026 Analog Devices Inc.
 */
#ifndef __ADI_OTP_PUBLIC_H
#define __ADI_OTP_PUBLIC_H

#include <linux/bitops.h>
#include <linux/types.h>

struct udevice;

/* ------------------------------------------------------------------------
 * The "fuse writebuff" buffer (doc/board/adi/adsp-otp.rst). Little-endian.
 */
#define ADI_OTP_BUF_MAGIC		0x50544f41	/* "AOTP" */
#define ADI_OTP_BUF_VERSION		1
#define ADI_OTP_BUF_MAX_SIZE		0x10000
/* The buffer's size, and each entry's data, are multiples of this */
#define ADI_OTP_BUF_ALIGN		4

/* Generic flags, bits 0-15 */
#define ADI_OTP_BUF_FLAG_LOCK		BIT(0)	/* lock the part at the end */
#define ADI_OTP_BUF_FLAG_DRY_RUN	BIT(1)	/* validate and report only */
#define ADI_OTP_BUF_FLAGS_CORE		GENMASK(15, 0)
/* Bits 16-31 belong to the SoC (struct adi_otp_soc.flags) */
#define ADI_OTP_BUF_FLAGS_SOC		GENMASK(31, 16)

/**
 * struct adi_otp_buf_hdr - header of a writebuff buffer
 * @magic: ADI_OTP_BUF_MAGIC
 * @version: ADI_OTP_BUF_VERSION
 * @hdr_size: sizeof(struct adi_otp_buf_hdr), 32
 * @soc_id: the SoC family the buffer is for (struct adi_otp_soc.id)
 * @flags: ADI_OTP_BUF_FLAG_* and SoC flags
 * @total_size: size of the whole buffer in bytes, header included
 * @num_entries: number of entries following the header
 * @crc32: CRC-32 (as zlib's crc32()) of @total_size bytes, computed with
 *	this field set to 0
 * @reserved: 0
 */
struct adi_otp_buf_hdr {
	__le32 magic;
	__le16 version;
	__le16 hdr_size;
	__le32 soc_id;
	__le32 flags;
	__le32 total_size;
	__le32 num_entries;
	__le32 crc32;
	__le32 reserved;
};

/* struct adi_otp_buf_entry.type */
#define ADI_OTP_ENTRY_DATA		1	/* bytes at an OTP offset */
#define ADI_OTP_ENTRY_COUNTER		2	/* target value of a counter */

/**
 * struct adi_otp_buf_entry - one entry of a writebuff buffer
 * @type: ADI_OTP_ENTRY_*
 * @reserved: 0
 * @offset: DATA: OTP byte offset, a multiple of the SoC's write size
 *	(2 bytes on the ADSP-SC58x). COUNTER: offset of the counter field
 * @length: DATA: number of data bytes, a non-zero multiple of the SoC's
 *	write size. COUNTER: 4
 *
 * Followed by @length bytes of data, padded with zeros to a multiple of
 * 4 bytes (ADI_OTP_BUF_ALIGN). DATA: the bytes to program, in OTP
 * address order (the first byte goes to @offset, the next to @offset + 1,
 * ...). COUNTER: the target count, a __le32.
 */
struct adi_otp_buf_entry {
	__le16 type;
	__le16 reserved;
	__le32 offset;
	__le32 length;
};

/* ------------------------------------------------------------------------
 * Driver interface
 */

/**
 * adi_otp_get_device() - find an ADI OTP device
 * @index: 0 for the first one, ...
 * @devp: the device, probed
 *
 * Return: 0 or -ENODEV.
 */
int adi_otp_get_device(unsigned int index, struct udevice **devp);

/**
 * adi_otp_write_buffer() - check and carry out a writebuff buffer
 * @dev: the OTP device
 * @buf: the buffer
 *
 * Nothing is programmed unless the whole buffer is valid and the SoC's
 * rules accept the resulting OTP content.
 *
 * Return: 0 or -errno.
 */
int adi_otp_write_buffer(struct udevice *dev, const void *buf);

/**
 * adi_otp_program_word() - program one 32-bit OTP word, under the same
 * rules as a writebuff buffer
 * @dev: the OTP device
 * @word: word index in the OTP area (byte offset / 4)
 * @val: value
 *
 * Return: 0 or -errno.
 */
int adi_otp_program_word(struct udevice *dev, unsigned int word, u32 val);

/**
 * adi_otp_read_word() - read one 32-bit OTP word
 * @dev: the OTP device
 * @word: word index in the OTP area (byte offset / 4)
 * @val: the value; reserved words read as 0
 *
 * Return: 0 or -errno.
 */
int adi_otp_read_word(struct udevice *dev, unsigned int word, u32 *val);

#endif /* __ADI_OTP_PUBLIC_H */
