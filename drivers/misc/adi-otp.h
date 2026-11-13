/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADI ADSP OTP driver: the interface between the generic part (adi-otp.c)
 * and the SoC families (adi-otp-<soc>.c). See doc/board/adi/adsp-otp.rst.
 *
 * adi-otp.c is the U-Boot driver. It knows no SoC: it parses requests (a
 * "fuse writebuff" buffer or a "fuse prog" word), applies the rules every
 * OTP shares from the SoC's data (struct adi_otp_soc), and owns the
 * sequence: check, report, apply, verify, lock. A SoC family provides its
 * data and its operations (struct adi_otp_soc_ops): how the OTP is read,
 * its own rules, and how a checked request is programmed.
 *
 * Adding a SoC family: an adi-otp-<soc>.c with its data, which starts with
 * its struct adi_otp_soc and may go on with whatever the SoC code needs,
 * and its compatibles in adi-otp.c.
 *
 * Copyright 2026 Analog Devices Inc.
 */
#ifndef __ADI_OTP_H
#define __ADI_OTP_H

#include <adi_otp.h>
#include <linux/bitops.h>
#include <linux/types.h>

struct udevice;

/* ------------------------------------------------------------------------
 * The SoC's data
 */

/* struct adi_otp_field.kind */
enum adi_otp_kind {
	ADI_OTP_DATA,		/* bytes, written with DATA entries */
	ADI_OTP_VALUE,		/* a one-word value, written with DATA entries */
	ADI_OTP_COUNTER,	/* a counter, advanced with COUNTER entries */
	ADI_OTP_READ_ONLY,	/* not written through this driver */
};

/**
 * struct adi_otp_field - one field of the OTP area
 * @name: field name, as in the SoC's OTP layout
 * @offset: byte offset in the OTP area, 4-byte aligned
 * @words: size in 32-bit words
 * @kind: what the field holds (ADI_OTP_*)
 * @val_mask: ADI_OTP_VALUE: the bits the value may use
 * @counter_max: ADI_OTP_COUNTER: the largest value
 * @soc_data: for the SoC code: its own data on the field, in a structure
 *	of its own (e.g. how its boot ROM programs the field), or NULL; not
 *	used by adi-otp.c
 */
struct adi_otp_field {
	const char *name;
	u16 offset;
	u8 words;
	u8 kind;
	u32 val_mask;
	u32 counter_max;
	const void *soc_data;
};

/**
 * struct adi_otp_guard - bytes that must never be written through this
 * driver, although they lie inside a writable field
 * @offset: byte offset, a multiple of the SoC's write size
 * @bytes: size in bytes, a multiple of the SoC's write size
 * @reason: why the bytes are guarded, for diagnostics
 */
struct adi_otp_guard {
	u16 offset;
	u16 bytes;
	const char *reason;
};

/* What adi_otp_soc_ops.lock_state() reports */
enum adi_otp_lock_state {
	ADI_OTP_OPEN,		/* not locked */
	ADI_OTP_LOCK_PENDING,	/* lock programmed, effective at reset */
	ADI_OTP_LOCKED,		/* locked */
	ADI_OTP_UNLOCKED,	/* "unlocked" (failure analysis) part */
};

/* ------------------------------------------------------------------------
 * A request
 */

/**
 * struct adi_otp_request - one write request: the OTP before, the data
 * asked for and the result, built and checked before anything is
 * programmed
 * @flags: buffer flags (ADI_OTP_BUF_FLAG_* and SoC flags)
 * @cur: current OTP content, an image of the OTP area in OTP byte order
 *	(byte n at offset n)
 * @want: requested content; write units that are 0 are not requested
 * @merged: the content after the request: @cur plus @delta
 * @delta: the write units to program: requested and blank in @cur
 * @readback: what the OTP reads back once programmed, filled in by
 *	adi_otp_verify_field()
 * @units: number of write units in @delta
 * @counter: the counter field a COUNTER entry advances, or NULL
 * @count: current value of @counter
 * @new_count: requested value of @counter
 *
 * The SoC's prepare() sees @cur, @want and @flags and may add data with
 * adi_otp_request_add(); its check() and apply() see the whole request,
 * read-only. @merged, @delta and @units are valid from check() on.
 */
struct adi_otp_request {
	u32 flags;
	u32 *cur;
	u32 *want;
	u32 *merged;
	u32 *delta;
	u32 *readback;
	unsigned int units;
	const struct adi_otp_field *counter;
	u32 count;
	u32 new_count;
};

/* ------------------------------------------------------------------------
 * The SoC's operations
 */

/**
 * struct adi_otp_soc_ops - what a SoC family does
 * @probe: set up the SoC's state (adi_otp_set_soc_priv())
 * @lock_state: an enum adi_otp_lock_state, or -errno
 * @read: read @words 32-bit words at byte @offset, a range within one
 *	field, into @data (anywhere, e.g. on the stack), in OTP byte order;
 *	whatever cannot be read in the current state reads as 0
 * @read_counter: read the value of the counter @field
 * @prepare: optional: add data to a request, before the generic check,
 *	e.g. copies requested by SoC flags
 * @check: optional: the SoC's own rules for a request, after the generic
 *	ones, e.g. that the part still boots and the conditions for locking.
 *	It can only refuse a request, never relax the generic rules.
 * @apply: program a checked request: the write units of @req->delta and,
 *	if @req->counter, the counter to @req->new_count, in the order the
 *	SoC needs, with whatever must surround it (e.g. stopping other
 *	cores). It may verify as it goes (adi_otp_verify_field()); the
 *	generic code verifies the whole request afterwards. Never locks.
 * @lock: lock the part; called by the generic code once the request has
 *	been applied and verified
 */
struct adi_otp_soc_ops {
	int (*probe)(struct udevice *dev);
	int (*lock_state)(struct udevice *dev);
	int (*read)(struct udevice *dev, unsigned int offset, u32 *data,
		    unsigned int words);
	int (*read_counter)(struct udevice *dev,
			    const struct adi_otp_field *field, u32 *count);
	int (*prepare)(struct udevice *dev, struct adi_otp_request *req);
	int (*check)(struct udevice *dev, const struct adi_otp_request *req);
	int (*apply)(struct udevice *dev, const struct adi_otp_request *req);
	int (*lock)(struct udevice *dev);
};

/**
 * struct adi_otp_soc - a SoC family
 * @name: SoC family name, for messages
 * @id: SoC family identifier of writebuff buffers
 * @flags: the SoC flags (bits 16-31) it accepts
 * @fields: OTP fields, sorted by offset, not overlapping
 * @nfields: number of @fields
 * @size: size of the OTP area: end of the last field
 * @write_size: the OTP's write unit in bytes (1, 2 or 4): buffer data
 *	must be aligned to it and a multiple of it, and each unit can be
 *	programmed once
 * @guards: bytes never to be written
 * @nguards: number of @guards
 * @ops: the SoC's operations
 */
struct adi_otp_soc {
	const char *name;
	u32 id;
	u32 flags;
	const struct adi_otp_field *fields;
	unsigned int nfields;
	unsigned int size;
	unsigned int write_size;
	const struct adi_otp_guard *guards;
	unsigned int nguards;
	const struct adi_otp_soc_ops *ops;
};

/* ------------------------------------------------------------------------
 * Helpers for the SoC code, provided by adi-otp.c
 */

static inline unsigned int adi_otp_field_bytes(const struct adi_otp_field *f)
{
	return f->words * sizeof(u32);
}

/* The word of an image (indexed like the OTP area) at byte @offset */
static inline u32 adi_otp_word(const u32 *image, unsigned int offset)
{
	return image[offset / 4];
}

const struct adi_otp_soc *adi_otp_get_soc(struct udevice *dev);
void *adi_otp_soc_priv(struct udevice *dev);
void adi_otp_set_soc_priv(struct udevice *dev, void *soc_priv);
const struct adi_otp_field *adi_otp_field_at(const struct adi_otp_soc *soc,
					     unsigned int pos);
int adi_otp_request_add(struct udevice *dev, struct adi_otp_request *req,
			unsigned int offset, const void *data,
			unsigned int len);
bool adi_otp_request_programs(const struct adi_otp_request *req,
			      unsigned int offset, unsigned int bytes);
int adi_otp_verify_field(struct udevice *dev,
			 const struct adi_otp_request *req,
			 const struct adi_otp_field *field);

#endif /* __ADI_OTP_H */
