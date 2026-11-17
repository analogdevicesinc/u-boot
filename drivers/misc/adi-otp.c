// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ADI ADSP OTP (One-Time Programmable memory) driver.
 *
 * This is the U-Boot driver and the part shared by every SoC family; the
 * family code (adi-otp-<soc>.c, struct adi_otp_soc) reads and programs the
 * OTP and adds its own rules. The buffer format and the rules are
 * documented in doc/board/adi/adsp-otp.rst.
 *
 * A request (a "fuse writebuff" buffer or a "fuse prog" word) goes
 * through a fixed sequence, and nothing is programmed unless all of it is
 * valid:
 *  1. Check: the part must be open, and the whole request is checked
 *     against the current OTP content:
 *     - The OTP is programmed in write units of the SoC's write size, and
 *       each unit can be programmed once. Requested units that are 0 or
 *       equal to the current value are skipped; any other must still be
 *       blank (0).
 *     - Only units of writable fields, outside the SoC's guarded bytes,
 *       may be written, value fields only within their mask; counters
 *       only count up, to their maximum.
 *     - Then the SoC's check(), which can only refuse more.
 *  2. Report what would be done; a dry run stops here.
 *  3. The SoC's apply() programs the request.
 *  4. The whole OTP area is read back and compared with the expected
 *     content.
 *  5. If requested, the part is locked, and the lock read back.
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <dm.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <misc.h>
#include <dm/device_compat.h>
#include <linux/string.h>
#include <u-boot/crc.h>
#include <u-boot/schedule.h>

#include "adi-otp.h"

/**
 * struct adi_otp_priv - driver state, one per OTP device
 * @soc: the SoC family
 * @soc_priv: the SoC code's state
 */
struct adi_otp_priv {
	const struct adi_otp_soc *soc;
	void *soc_priv;
};

/* The device's SoC family */
const struct adi_otp_soc *adi_otp_get_soc(struct udevice *dev)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);

	return priv->soc;
}

void *adi_otp_soc_priv(struct udevice *dev)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);

	return priv->soc_priv;
}

void adi_otp_set_soc_priv(struct udevice *dev, void *soc_priv)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);

	priv->soc_priv = soc_priv;
}

/* ------------------------------------------------------------------------
 * Fields and images
 */

/* The field containing byte @pos, or NULL for a reserved gap */
const struct adi_otp_field *adi_otp_field_at(const struct adi_otp_soc *soc,
					     unsigned int pos)
{
	unsigned int i;

	for (i = 0; i < soc->nfields; i++) {
		const struct adi_otp_field *f = &soc->fields[i];

		if (pos >= f->offset && pos < f->offset + adi_otp_field_bytes(f))
			return f;
	}

	return NULL;
}

static const struct adi_otp_guard *adi_otp_guard_at(const struct adi_otp_soc *soc,
						    unsigned int pos)
{
	unsigned int i;

	for (i = 0; i < soc->nguards; i++) {
		const struct adi_otp_guard *g = &soc->guards[i];

		if (pos >= g->offset && pos < g->offset + g->bytes)
			return g;
	}

	return NULL;
}

static bool adi_otp_writable(const struct adi_otp_field *f)
{
	return f->kind == ADI_OTP_DATA || f->kind == ADI_OTP_VALUE;
}

/*
 * Images hold the OTP area in OTP byte order: byte n is at byte offset n.
 * A write unit is @size (1, 2 or 4) bytes at byte @pos, a multiple of
 * @size, taken as a little-endian value.
 */
static u32 adi_otp_unit(const u32 *image, unsigned int pos, unsigned int size)
{
	const u8 *b = (const u8 *)image + pos;
	u32 val = 0;

	while (size--)
		val = (val << BITS_PER_BYTE) | b[size];

	return val;
}

static void adi_otp_set_unit(u32 *image, unsigned int pos, unsigned int size,
			     u32 val)
{
	u8 *b = (u8 *)image + pos;

	for (; size; size--, val >>= BITS_PER_BYTE)
		*b++ = val;
}

/* Read field @f into its place in @image */
static int adi_otp_read_field(struct udevice *dev,
			      const struct adi_otp_field *f, u32 *image)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);

	return priv->soc->ops->read(dev, f->offset,
				    image + f->offset / sizeof(u32), f->words);
}

/* Read every field into @image (reserved gaps read as 0) */
static int adi_otp_read_image(struct udevice *dev, u32 *image)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc = priv->soc;
	unsigned int i;
	int ret = 0;

	memset(image, 0, soc->size);
	for (i = 0; i < soc->nfields && !ret; i++)
		ret = adi_otp_read_field(dev, &soc->fields[i], image);

	return ret;
}

/* ------------------------------------------------------------------------
 * Building a request
 */

static void adi_otp_request_free(struct udevice *dev,
				 struct adi_otp_request *req)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	unsigned int size = priv->soc->size;
	u32 *images[] = {
		req->cur, req->want, req->merged, req->delta, req->readback,
	};
	unsigned int i;

	/* The images may hold key material */
	for (i = 0; i < ARRAY_SIZE(images); i++) {
		if (images[i])
			memset(images[i], 0, size);
		free(images[i]);
	}
	memset(req, 0, sizeof(*req));
}

static int adi_otp_request_init(struct udevice *dev,
				struct adi_otp_request *req, u32 flags)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	unsigned int size = priv->soc->size;

	memset(req, 0, sizeof(*req));
	req->flags = flags;
	req->cur = calloc(1, size);
	req->want = calloc(1, size);
	req->merged = calloc(1, size);
	req->delta = calloc(1, size);
	req->readback = calloc(1, size);
	if (!req->cur || !req->want || !req->merged || !req->delta ||
	    !req->readback) {
		adi_otp_request_free(dev, req);
		return -ENOMEM;
	}

	return adi_otp_read_image(dev, req->cur);
}

/**
 * adi_otp_request_add() - request bytes at an OTP offset
 * @dev: the OTP device
 * @req: the request
 * @offset: OTP byte offset, a multiple of the SoC's write size
 * @data: the bytes, in OTP address order (lowest address first)
 * @len: number of bytes, a non-zero multiple of the SoC's write size
 *
 * Write units that are 0 are not requested. The others must lie in
 * writable fields, outside the guarded bytes, and agree with whatever the
 * request already holds for them.
 *
 * Return: 0 or -errno.
 */
int adi_otp_request_add(struct udevice *dev, struct adi_otp_request *req,
			unsigned int offset, const void *data,
			unsigned int len)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc = priv->soc;
	unsigned int ws = soc->write_size;
	const u8 *bytes = data;
	unsigned int i, j;

	if (!len || len % ws || offset % ws || offset > soc->size ||
	    len > soc->size - offset) {
		dev_err(dev, "bad data range 0x%x+0x%x (multiples of %u bytes, within 0x%x)\n",
			offset, len, ws, soc->size);
		return -EINVAL;
	}

	for (i = 0; i < len; i += ws) {
		unsigned int pos = offset + i;
		const struct adi_otp_field *f;
		const struct adi_otp_guard *g;
		u32 val = 0, old;

		for (j = ws; j; j--)
			val = (val << BITS_PER_BYTE) | bytes[i + j - 1];
		if (!val)
			continue;

		f = adi_otp_field_at(soc, pos);
		if (!f || !adi_otp_writable(f)) {
			dev_err(dev, "0x%03x (%s) is not writable%s\n", pos,
				f ? f->name : "reserved",
				f && f->kind == ADI_OTP_COUNTER ?
				": use a COUNTER entry" : "");
			return -EACCES;
		}

		g = adi_otp_guard_at(soc, pos);
		if (g) {
			dev_err(dev, "0x%03x (%s) is protected: %s\n", pos,
				f->name, g->reason);
			return -EACCES;
		}

		old = adi_otp_unit(req->want, pos, ws);
		if (old && old != val) {
			dev_err(dev, "0x%03x (%s) is requested twice with different values\n",
				pos, f->name);
			return -EINVAL;
		}
		adi_otp_set_unit(req->want, pos, ws, val);
	}

	return 0;
}

/* Request a counter value */
static int adi_otp_request_counter(struct udevice *dev,
				   struct adi_otp_request *req,
				   unsigned int offset, u32 new_count)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_field *f = adi_otp_field_at(priv->soc, offset);
	u32 count;
	int ret;

	if (!f || f->offset != offset || f->kind != ADI_OTP_COUNTER) {
		dev_err(dev, "0x%03x is not a counter\n", offset);
		return -EINVAL;
	}
	if (req->counter && (req->counter != f || req->new_count != new_count)) {
		dev_err(dev, "%s is requested twice\n", f->name);
		return -EINVAL;
	}

	ret = priv->soc->ops->read_counter(dev, f, &count);
	if (ret)
		return ret;

	if (new_count < count || new_count > f->counter_max) {
		dev_err(dev, "%s: cannot go from %u to %u (counts up to %u, never down)\n",
			f->name, count, new_count, f->counter_max);
		return -EINVAL;
	}

	req->counter = f;
	req->count = count;
	req->new_count = new_count;
	return 0;
}

/**
 * adi_otp_request_programs() - whether a request programs any byte of a
 * range
 * @req: a request, from the SoC's check() on
 * @offset: OTP byte offset
 * @bytes: size of the range in bytes
 *
 * Return: true if a byte in the range will be programmed.
 */
bool adi_otp_request_programs(const struct adi_otp_request *req,
			      unsigned int offset, unsigned int bytes)
{
	return memchr_inv((const u8 *)req->delta + offset, 0, bytes);
}

/*
 * Compare the request with the current content: compute the write units
 * to program and the resulting content, and check value fields.
 */
static int adi_otp_request_finalize(struct udevice *dev,
				    struct adi_otp_request *req)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc = priv->soc;
	unsigned int ws = soc->write_size;
	unsigned int pos, i;

	req->units = 0;
	memset(req->delta, 0, soc->size);
	for (pos = 0; pos < soc->size; pos += ws) {
		u32 want = adi_otp_unit(req->want, pos, ws);
		u32 cur = adi_otp_unit(req->cur, pos, ws);

		if (!want || want == cur)
			continue;
		if (cur) {
			const struct adi_otp_field *f = adi_otp_field_at(soc, pos);

			/* No values: the field may hold key material */
			dev_err(dev, "0x%03x (%s byte %u) is already programmed with a different value\n",
				pos, f->name, pos - f->offset);
			return -EEXIST;
		}
		adi_otp_set_unit(req->delta, pos, ws, want);
		req->units++;
	}

	for (i = 0; i < soc->size / sizeof(u32); i++)
		req->merged[i] = req->cur[i] | req->delta[i];

	for (i = 0; i < soc->nfields; i++) {
		const struct adi_otp_field *f = &soc->fields[i];

		if (f->kind != ADI_OTP_VALUE ||
		    !adi_otp_request_programs(req, f->offset, sizeof(u32)))
			continue;
		if (adi_otp_word(req->merged, f->offset) & ~f->val_mask) {
			dev_err(dev, "%s: value out of range (mask 0x%x)\n",
				f->name, f->val_mask);
			return -EINVAL;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------------
 * Carrying out a request
 */

static int adi_otp_check_open(struct udevice *dev)
{
	static const char * const reasons[] = {
		[ADI_OTP_LOCK_PENDING] = "its lock is programmed (effective at the next reset)",
		[ADI_OTP_LOCKED] = "it is locked",
		[ADI_OTP_UNLOCKED] = "it is in the unlocked state",
	};
	struct adi_otp_priv *priv = dev_get_priv(dev);
	int state = priv->soc->ops->lock_state(dev);

	if (state < 0)
		return state;
	if (state == ADI_OTP_OPEN)
		return 0;

	dev_err(dev, "the OTP cannot be programmed: %s\n",
		(unsigned int)state < ARRAY_SIZE(reasons) && reasons[state] ?
		reasons[state] : "unknown state");
	return -EROFS;
}

static void adi_otp_report(struct udevice *dev,
			   const struct adi_otp_request *req)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc = priv->soc;
	unsigned int ws = soc->write_size;
	unsigned int i, pos, n;

	printf("%s OTP: %u byte(s) to program, in %u-byte units\n", soc->name,
	       req->units * ws, ws);
	for (i = 0; i < soc->nfields; i++) {
		const struct adi_otp_field *f = &soc->fields[i];

		for (n = 0, pos = f->offset;
		     pos < f->offset + adi_otp_field_bytes(f); pos += ws)
			n += !!adi_otp_unit(req->delta, pos, ws);
		if (n)
			printf("  %-17s %3u byte(s)\n", f->name, n * ws);
	}
	if (req->counter && req->new_count != req->count)
		printf("  %-17s %u -> %u\n", req->counter->name, req->count,
		       req->new_count);
	if (req->flags & ADI_OTP_BUF_FLAG_LOCK)
		printf("  lock the part\n");
}

/**
 * adi_otp_verify_field() - check that a field reads back as a request
 * expects
 * @dev: the OTP device
 * @req: the request, applied
 * @f: the field
 *
 * A field reads back (into @req->readback) as @req->merged, except the
 * counter that @req advances, which must read @req->new_count. Mismatches
 * are reported by position, not value: the field may hold key material.
 *
 * Return: 0, -EIO on a mismatch, or -errno.
 */
int adi_otp_verify_field(struct udevice *dev,
			 const struct adi_otp_request *req,
			 const struct adi_otp_field *f)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	unsigned int ws = priv->soc->write_size;
	const u32 *want = req->merged + f->offset / sizeof(u32);
	const u32 *got = req->readback + f->offset / sizeof(u32);
	unsigned int pos;
	u32 count;
	int ret;

	if (f == req->counter) {
		ret = priv->soc->ops->read_counter(dev, f, &count);
		if (!ret && count != req->new_count) {
			dev_err(dev, "%s reads %u, not %u\n", f->name, count,
				req->new_count);
			ret = -EIO;
		}
		return ret;
	}

	ret = adi_otp_read_field(dev, f, req->readback);
	if (!ret && memcmp(got, want, adi_otp_field_bytes(f))) {
		for (pos = 0; pos < adi_otp_field_bytes(f); pos += ws)
			if (adi_otp_unit(want, pos, ws) !=
			    adi_otp_unit(got, pos, ws))
				dev_err(dev, "%s bytes %u-%u do not read back as expected\n",
					f->name, pos, pos + ws - 1);
		ret = -EIO;
	}

	return ret;
}

/* Lock the part and check that the lock reads back */
static int adi_otp_lock(struct udevice *dev)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc_ops *ops = priv->soc->ops;
	int ret;

	schedule();
	ret = ops->lock(dev);
	if (!ret) {
		ret = ops->lock_state(dev);
		if (ret == ADI_OTP_LOCK_PENDING) {
			printf("Lock programmed: the part is locked from the next reset on\n");
			ret = 0;
		} else if (ret == ADI_OTP_LOCKED) {
			printf("The part is locked\n");
			ret = 0;
		} else if (ret >= 0) {
			dev_err(dev, "the lock does not read back as programmed\n");
			ret = -EIO;
		}
	}
	if (ret)
		dev_err(dev, "locking failed; the lock may be programmed\n");

	return ret;
}

/* Check a whole request, then carry it out */
static int adi_otp_run(struct udevice *dev, struct adi_otp_request *req)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc = priv->soc;
	bool lock = req->flags & ADI_OTP_BUF_FLAG_LOCK;
	unsigned int i;
	int ret;

	/* 1. Check */
	if (soc->ops->prepare) {
		ret = soc->ops->prepare(dev, req);
		if (ret)
			return ret;
	}

	ret = adi_otp_request_finalize(dev, req);
	if (ret)
		return ret;

	if (soc->ops->check) {
		ret = soc->ops->check(dev, req);
		if (ret)
			return ret;
	}

	/* 2. Report */
	adi_otp_report(dev, req);
	if (!req->units && !lock &&
	    (!req->counter || req->new_count == req->count)) {
		printf("Nothing to program\n");
		return 0;
	}
	if (req->flags & ADI_OTP_BUF_FLAG_DRY_RUN) {
		printf("Dry run: nothing programmed\n");
		return 0;
	}

	/* 3. Apply */
	ret = soc->ops->apply(dev, req);
	if (ret) {
		dev_err(dev, "programming stopped: what was programmed before the failure stays programmed\n");
		return ret;
	}

	/* 4. Verify all of it: the request, and nothing else changed */
	for (i = 0; i < soc->nfields; i++) {
		schedule();
		ret = adi_otp_verify_field(dev, req, &soc->fields[i]);
		if (ret) {
			dev_err(dev, "the OTP does not read back as expected%s\n",
				lock ? ": not locking" : "");
			return ret;
		}
	}

	/* 5. Lock */
	if (lock) {
		ret = adi_otp_lock(dev);
		if (ret)
			return ret;
	}

	printf("OTP programmed and verified\n");
	return 0;
}

/* ------------------------------------------------------------------------
 * writebuff buffers
 */

static int adi_otp_parse_header(struct udevice *dev,
				const struct adi_otp_buf_hdr *hdr, u32 *size)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc = priv->soc;
	u32 flags = le32_to_cpu(hdr->flags);

	if (le32_to_cpu(hdr->magic) != ADI_OTP_BUF_MAGIC) {
		dev_err(dev, "not an ADI OTP buffer (bad magic)\n");
		return -EINVAL;
	}
	if (le16_to_cpu(hdr->version) != ADI_OTP_BUF_VERSION ||
	    le16_to_cpu(hdr->hdr_size) != sizeof(*hdr) || hdr->reserved) {
		dev_err(dev, "unsupported buffer version %u (header size %u)\n",
			le16_to_cpu(hdr->version), le16_to_cpu(hdr->hdr_size));
		return -EINVAL;
	}
	if (le32_to_cpu(hdr->soc_id) != soc->id) {
		dev_err(dev, "buffer is for SoC id 0x%08x, not %s (0x%08x)\n",
			le32_to_cpu(hdr->soc_id), soc->name, soc->id);
		return -EINVAL;
	}
	if ((flags & ADI_OTP_BUF_FLAGS_CORE &
	     ~(ADI_OTP_BUF_FLAG_LOCK | ADI_OTP_BUF_FLAG_DRY_RUN)) ||
	    (flags & ADI_OTP_BUF_FLAGS_SOC & ~soc->flags)) {
		dev_err(dev, "unknown buffer flags 0x%08x\n", flags);
		return -EINVAL;
	}

	*size = le32_to_cpu(hdr->total_size);
	if (*size < sizeof(*hdr) || *size > ADI_OTP_BUF_MAX_SIZE ||
	    *size % ADI_OTP_BUF_ALIGN) {
		dev_err(dev, "bad buffer size %u\n", *size);
		return -EINVAL;
	}

	return 0;
}

static int adi_otp_parse_entries(struct udevice *dev,
				 struct adi_otp_request *req, const u8 *buf,
				 u32 size, u32 count)
{
	u32 pos = sizeof(struct adi_otp_buf_hdr);
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		const struct adi_otp_buf_entry *e;
		u32 offset, len, padded, new_count;

		if (size - pos < sizeof(*e)) {
			dev_err(dev, "entry %u: truncated\n", i);
			return -EINVAL;
		}
		e = (const void *)(buf + pos);
		pos += sizeof(*e);
		offset = le32_to_cpu(e->offset);
		len = le32_to_cpu(e->length);
		padded = ALIGN(len, ADI_OTP_BUF_ALIGN);
		if (e->reserved || padded < len || size - pos < padded) {
			dev_err(dev, "entry %u: bad entry\n", i);
			return -EINVAL;
		}

		switch (le16_to_cpu(e->type)) {
		case ADI_OTP_ENTRY_DATA:
			ret = adi_otp_request_add(dev, req, offset, buf + pos,
						  len);
			break;
		case ADI_OTP_ENTRY_COUNTER:
			if (len != sizeof(__le32)) {
				ret = -EINVAL;
				break;
			}
			new_count = le32_to_cpu(*(const __le32 *)(buf + pos));
			ret = adi_otp_request_counter(dev, req, offset,
						      new_count);
			break;
		default:
			dev_err(dev, "entry %u: unknown type %u\n", i,
				le16_to_cpu(e->type));
			ret = -EINVAL;
		}
		if (ret) {
			dev_err(dev, "entry %u rejected\n", i);
			return ret;
		}

		/* Padding must be zero */
		for (; len < padded; len++) {
			if (buf[pos + len]) {
				dev_err(dev, "entry %u: non-zero padding\n", i);
				return -EINVAL;
			}
		}
		pos += padded;
	}

	if (pos != size) {
		dev_err(dev, "%u stray bytes after the last entry\n",
			size - pos);
		return -EINVAL;
	}

	return 0;
}

/* ------------------------------------------------------------------------
 * Interface (include/adi_otp.h)
 */

int adi_otp_write_buffer(struct udevice *dev, const void *buf)
{
	struct adi_otp_buf_hdr hdr;
	struct adi_otp_request req;
	u32 size, crc;
	int ret;

	memcpy(&hdr, buf, sizeof(hdr));
	ret = adi_otp_parse_header(dev, &hdr, &size);
	if (ret)
		return ret;

	/* CRC over the whole buffer with the CRC field as 0 */
	hdr.crc32 = 0;
	crc = crc32(0, (const u8 *)&hdr, sizeof(hdr));
	crc = crc32(crc, (const u8 *)buf + sizeof(hdr), size - sizeof(hdr));
	if (crc != le32_to_cpu(((const struct adi_otp_buf_hdr *)buf)->crc32)) {
		dev_err(dev, "buffer CRC mismatch\n");
		return -EBADMSG;
	}

	ret = adi_otp_check_open(dev);
	if (ret)
		return ret;

	ret = adi_otp_request_init(dev, &req, le32_to_cpu(hdr.flags));
	if (!ret)
		ret = adi_otp_parse_entries(dev, &req, buf, size,
					    le32_to_cpu(hdr.num_entries));
	if (!ret)
		ret = adi_otp_run(dev, &req);
	adi_otp_request_free(dev, &req);

	return ret;
}

int adi_otp_program_word(struct udevice *dev, unsigned int word, u32 val)
{
	struct adi_otp_request req;
	/* The word's bytes in OTP address order */
	__le32 data = cpu_to_le32(val);
	int ret;

	ret = adi_otp_check_open(dev);
	if (ret)
		return ret;

	ret = adi_otp_request_init(dev, &req, 0);
	if (!ret)
		ret = adi_otp_request_add(dev, &req, word * sizeof(u32),
					  &data, sizeof(data));
	if (!ret)
		ret = adi_otp_run(dev, &req);
	adi_otp_request_free(dev, &req);

	return ret;
}

int adi_otp_read_word(struct udevice *dev, unsigned int word, u32 *val)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	unsigned int offset = word * sizeof(u32);

	if (word >= priv->soc->size / sizeof(u32))
		return -EINVAL;

	*val = 0;
	if (!adi_otp_field_at(priv->soc, offset))
		return 0;

	return priv->soc->ops->read(dev, offset, val, 1);
}

/* misc_read(): bytes of the OTP area */
static int adi_otp_misc_read(struct udevice *dev, int offset, void *buf,
			     int size)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	u32 *image;
	int ret;

	if (offset < 0 || size < 0 || offset > priv->soc->size ||
	    size > priv->soc->size - offset)
		return -EINVAL;

	image = calloc(1, priv->soc->size);
	if (!image)
		return -ENOMEM;

	ret = adi_otp_read_image(dev, image);
	if (!ret)
		memcpy(buf, (u8 *)image + offset, size);

	/* May hold key material */
	memset(image, 0, priv->soc->size);
	free(image);

	return ret ? ret : size;
}

static const struct misc_ops adi_otp_misc_ops = {
	.read = adi_otp_misc_read,
};

int adi_otp_get_device(unsigned int index, struct udevice **devp)
{
	struct udevice *dev;

	uclass_foreach_dev_probe(UCLASS_MISC, dev) {
		if (dev->driver != DM_DRIVER_GET(adi_otp))
			continue;
		if (!index--) {
			*devp = dev;
			return 0;
		}
	}

	return -ENODEV;
}

/* ------------------------------------------------------------------------
 * Driver
 */

/*
 * The SoC data is checked by the unit tests (test/dm/adi_otp.c), not
 * here: it is the driver's own, constant
 */
static int adi_otp_probe(struct udevice *dev)
{
	struct adi_otp_priv *priv = dev_get_priv(dev);
	const struct adi_otp_soc *soc;

	/* The SoC family's data, which starts with its struct adi_otp_soc */
	soc = (const struct adi_otp_soc *)dev_get_driver_data(dev);
	if (!soc)
		return -ENODEV;	/* bound without a compatible */

	priv->soc = soc;

	return soc->ops->probe(dev);
}

/* The SoC families, by compatible */
static const struct udevice_id adi_otp_ids[] = {
#if IS_ENABLED(CONFIG_ADI_OTP_SC58X)
	{ .compatible = "adi,adsp-sc589-otp", .data = (ulong)&adi_sc58x_soc },
#endif
	{ }
};

U_BOOT_DRIVER(adi_otp) = {
	.name = "adi-otp",
	.id = UCLASS_MISC,
	.of_match = adi_otp_ids,
	.probe = adi_otp_probe,
	.priv_auto = sizeof(struct adi_otp_priv),
	.ops = &adi_otp_misc_ops,
};
