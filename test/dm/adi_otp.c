// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for the ADI ADSP OTP driver: the generic code (adi-otp.c) and the
 * ADSP-SC58x code (adi-otp-sc58x.c), on the emulated boot ROM of
 * drivers/misc/adi-otp-sc58x-sandbox.c. They cover the writebuff buffer
 * format, the programming rules (nothing is programmed unless the whole
 * request is valid), the SC58x boot-safety and lock rules, verification,
 * the counter, ROM failures and the anomaly workarounds; and the SoC data,
 * which the driver does not check itself.
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <adi_otp.h>
#include <dm.h>
#include <mapmem.h>
#include <misc.h>
#include <asm/test.h>
#include <asm/unaligned.h>
#include <dm/test.h>
#include <linux/string.h>
#include <test/test.h>
#include <test/ut.h>
#include <u-boot/crc.h>

#include "../../drivers/misc/adi-otp.h"

/* ADSP-SC58x OTP offsets */
#define HUK		0x000
#define EMU_KEY		0x1c8
#define RESERVED	0x1d8
#define PUBLIC_KEY0	0x200
#define BOOT_INFO	0x280
#define OTP_TIMING	0x2bc
#define ANTIROLL	0x2c0
#define GP1		0x300
#define BMD		0x340
#define STAGE_ID	0x374
#define FSN		0x380
#define FSN_BYTES	16
#define LOCK		0x48c
#define LOCK_BIT	BIT(0)
#define ANTIROLL_MAX	32

/* The OTP word (index) of an offset, as "fuse read" and "fuse prog" take */
#define WORD(offset)	((offset) / sizeof(u32))

/* boot_info words (ADI_ROM_OTP_BOOT_INFO) and bits */
#define BI(word)		(BOOT_INFO + (word) * sizeof(u32))
#define BI_CGU0			0
#define BI_CGU0_CTL_WEN		BIT(0)
#define BI_CGU1			1	/* CTL_MSEL in bits 6:0 */
#define BI_BCMD_SPI_MASTER	4
#define BI_BCMD_SPI_SLAVE	5
#define BI_BCMD_LINKPORT	6
#define BI_BCFG5		13
#define BI_BCFG5_DMC_EN		BIT(0)
/* Any boot command: a non-zero value */
#define BCMD			1

/* bootModeDisable bits; the emulated part boots in SPI master mode */
#define BMD_SPI_MASTER		BIT(0)
#define BMD_SPI_SLAVE		BIT(1)
#define BMD_OUT_OF_RANGE	BIT(8)	/* the value has 8 bits */

#define RCU_STAT	0x04
#define RCU_CRCTL	0x08

/* Buffer header values that are not valid */
#define BAD_CORE_FLAG		BIT(5)
#define BAD_SOC_FLAG		BIT(30)
#define BAD_ENTRY_TYPE		7

struct otp_buf {
	u8 data[512];
	u32 len;
	u32 count;
};

static void buf_init(struct otp_buf *b)
{
	memset(b, 0, sizeof(*b));
	b->len = sizeof(struct adi_otp_buf_hdr);
}

static void buf_entry(struct otp_buf *b, u16 type, u32 offset,
		      const void *data, u32 len)
{
	struct adi_otp_buf_entry e = {
		.type = cpu_to_le16(type),
		.offset = cpu_to_le32(offset),
		.length = cpu_to_le32(len),
	};

	memcpy(b->data + b->len, &e, sizeof(e));
	b->len += sizeof(e);
	memcpy(b->data + b->len, data, len);
	b->len += ALIGN(len, ADI_OTP_BUF_ALIGN);
	b->count++;
}

static void buf_data(struct otp_buf *b, u32 offset, const void *data, u32 len)
{
	buf_entry(b, ADI_OTP_ENTRY_DATA, offset, data, len);
}

static void buf_word(struct otp_buf *b, u32 offset, u32 val)
{
	__le32 v = cpu_to_le32(val);

	buf_data(b, offset, &v, sizeof(v));
}

static void buf_counter(struct otp_buf *b, u32 offset, u32 count)
{
	__le32 v = cpu_to_le32(count);

	buf_entry(b, ADI_OTP_ENTRY_COUNTER, offset, &v, sizeof(v));
}

static void buf_crc(struct otp_buf *b)
{
	struct adi_otp_buf_hdr *hdr = (void *)b->data;

	hdr->crc32 = 0;
	hdr->crc32 = cpu_to_le32(crc32(0, b->data, b->len));
}

static const void *buf_finish(struct otp_buf *b, u32 flags)
{
	struct adi_otp_buf_hdr *hdr = (void *)b->data;

	hdr->magic = cpu_to_le32(ADI_OTP_BUF_MAGIC);
	hdr->version = cpu_to_le16(ADI_OTP_BUF_VERSION);
	hdr->hdr_size = cpu_to_le16(sizeof(*hdr));
	hdr->soc_id = cpu_to_le32(ADI_OTP_SC58X_ID);
	hdr->flags = cpu_to_le32(flags);
	hdr->total_size = cpu_to_le32(b->len);
	hdr->num_entries = cpu_to_le32(b->count);
	buf_crc(b);

	return b->data;
}

static u32 *reg(ulong addr)
{
	return map_sysmem(addr, sizeof(u32));
}

/* A blank, open part booted in SPI master mode, and its OTP device */
static int setup(struct unit_test_state *uts, struct udevice **devp)
{
	sandbox_set_enable_memio(true);
	sandbox_adi_rom_init();
	ut_assertok(adi_otp_get_device(0, devp));

	return 0;
}

static const u8 test_key[16] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};

/* A valid buffer is programmed, verified and reads back */
static int dm_test_adi_otp_write(struct unit_test_state *uts)
{
	struct udevice *dev;
	struct otp_buf b;
	unsigned int calls;
	u8 rd[16], *otp;
	u32 val;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	/* bootModeDisable bit 1: SPI slave, not the mode the part boots in */
	buf_init(&b);
	buf_data(&b, GP1, test_key, sizeof(test_key));
	buf_word(&b, BMD, BMD_SPI_SLAVE);
	buf_counter(&b, ANTIROLL, 3);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	ut_asserteq_mem(test_key, otp + GP1, sizeof(test_key));
	ut_asserteq(BMD_SPI_SLAVE, otp[BMD]);
	/* Anomaly 20000084: the SHARC+ cores are held in reset */
	ut_asserteq(SANDBOX_ADI_SC58X_SHARCS,
		    *reg(SANDBOX_ADI_SC58X_RCU + RCU_CRCTL));
	/* The ROM is only called while it is executable */
	ut_asserteq(0, sandbox_adi_rom_unbracketed_calls());

	ut_asserteq(sizeof(rd), misc_read(dev, GP1, rd, sizeof(rd)));
	ut_asserteq_mem(test_key, rd, sizeof(rd));
	ut_assertok(adi_otp_read_word(dev, WORD(FSN), &val));
	ut_asserteq(get_unaligned_le32(otp + FSN), val);
	ut_assertok(adi_otp_read_word(dev, WORD(RESERVED), &val));
	ut_asserteq(0, val);

	/* The same buffer again changes nothing, and programs nothing */
	calls = sandbox_adi_rom_pgm_calls();
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	ut_asserteq(calls, sandbox_adi_rom_pgm_calls());

	/*
	 * Half-words can be added to a field later. otp_pgm() fails if given
	 * a count below the counter's (3): it is given the current count.
	 */
	buf_init(&b);
	buf_word(&b, GP1 + 0x10, 0x00010000);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	ut_asserteq(0x01, otp[GP1 + 0x12]);

	/* One word, as "fuse prog" does */
	ut_assertok(adi_otp_program_word(dev, WORD(GP1) + 8, 0xbeef));
	ut_asserteq(0xef, otp[GP1 + 0x20]);
	ut_asserteq(-EEXIST, adi_otp_program_word(dev, WORD(GP1) + 8, 0xdead));

	return 0;
}
DM_TEST(dm_test_adi_otp_write, UTF_SCAN_FDT);

/* Malformed buffers are rejected */
static int dm_test_adi_otp_format(struct unit_test_state *uts)
{
	struct adi_otp_buf_hdr *hdr;
	struct udevice *dev;
	struct otp_buf b;

	ut_assertok(setup(uts, &dev));

	buf_init(&b);
	buf_data(&b, GP1, test_key, sizeof(test_key));
	buf_finish(&b, 0);
	hdr = (void *)b.data;

	/* CRC */
	b.data[b.len - 1] ^= 1;
	ut_asserteq(-EBADMSG, adi_otp_write_buffer(dev, b.data));
	b.data[b.len - 1] ^= 1;

	/* Magic, SoC, flags, version */
	hdr->magic = 0;
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, b.data));
	buf_finish(&b, 0);
	hdr->soc_id = cpu_to_le32(0x12345678);
	buf_crc(&b);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, b.data));
	ut_asserteq(-EINVAL,
		    adi_otp_write_buffer(dev, buf_finish(&b, BAD_CORE_FLAG)));
	ut_asserteq(-EINVAL,
		    adi_otp_write_buffer(dev, buf_finish(&b, BAD_SOC_FLAG)));
	buf_finish(&b, 0);
	hdr->version = cpu_to_le16(ADI_OTP_BUF_VERSION + 1);
	buf_crc(&b);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, b.data));

	/* Entry count not matching the entries */
	buf_finish(&b, 0);
	hdr->num_entries = cpu_to_le32(b.count + 1);
	buf_crc(&b);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, b.data));
	buf_finish(&b, 0);
	hdr->num_entries = 0;
	buf_crc(&b);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, b.data));

	/* Unknown entry type */
	buf_init(&b);
	buf_entry(&b, BAD_ENTRY_TYPE, GP1, test_key, sizeof(u32));
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* Nothing was programmed */
	ut_asserteq(0, sandbox_adi_rom_pgm_calls());

	return 0;
}
DM_TEST(dm_test_adi_otp_format, UTF_SCAN_FDT);

/* The generic rules: a request is checked as a whole first */
static int dm_test_adi_otp_rules(struct unit_test_state *uts)
{
	static const u8 odd[3] = { 1, 2, 3 };
	struct udevice *dev;
	struct otp_buf b;
	u8 *otp;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	/* Reserved gap, guard, read-only field, counter by DATA */
	buf_init(&b);
	buf_word(&b, RESERVED, 1);
	ut_asserteq(-EACCES, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_word(&b, OTP_TIMING, 1);
	ut_asserteq(-EACCES, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_word(&b, FSN, 1);
	ut_asserteq(-EACCES, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_word(&b, ANTIROLL, 1);
	ut_asserteq(-EACCES, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* Alignment, value range, conflicting entries */
	buf_init(&b);
	buf_data(&b, GP1 + 1, test_key, 2);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_data(&b, GP1, odd, sizeof(odd));
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_word(&b, BMD, BMD_OUT_OF_RANGE);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_word(&b, GP1, 1);
	buf_word(&b, GP1, 2);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* The counter only counts up, to its maximum */
	buf_init(&b);
	buf_counter(&b, ANTIROLL, ANTIROLL_MAX + 1);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_counter(&b, ANTIROLL, 4);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	buf_init(&b);
	buf_counter(&b, ANTIROLL, 2);
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* Program part of gp1, then conflict with it */
	buf_init(&b);
	buf_word(&b, GP1, 0x1111);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* A valid entry and a conflicting one: nothing is programmed */
	buf_init(&b);
	buf_word(&b, GP1 + 4, 0x2222);
	buf_word(&b, GP1, 0x3333);
	ut_asserteq(-EEXIST, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	ut_asserteq(0, otp[GP1 + 4]);

	/* Blank (zero) data and equal half-words are fine: nothing to do */
	buf_init(&b);
	buf_word(&b, GP1 + 8, 0);
	buf_word(&b, GP1, 0x1111);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	/* Two requests programmed something: the counter and gp1 */
	ut_asserteq(2, sandbox_adi_rom_pgm_calls());

	return 0;
}
DM_TEST(dm_test_adi_otp_rules, UTF_SCAN_FDT);

/* A dry run checks everything and reports, but programs nothing */
static int dm_test_adi_otp_dry_run(struct unit_test_state *uts)
{
	struct udevice *dev;
	struct otp_buf b;
	u8 *otp;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	buf_init(&b);
	buf_data(&b, PUBLIC_KEY0, test_key, sizeof(test_key));
	buf_data(&b, EMU_KEY, test_key, sizeof(test_key));
	ut_assertok(adi_otp_write_buffer(dev,
					 buf_finish(&b, ADI_OTP_BUF_FLAG_DRY_RUN |
						    ADI_OTP_BUF_FLAG_LOCK |
						    ADI_OTP_SC58X_FLAG_FSN_COPY)));
	ut_asserteq(0, otp[PUBLIC_KEY0]);
	ut_asserteq(0, otp[GP1]);
	ut_asserteq(0, otp[LOCK]);
	ut_asserteq(0, sandbox_adi_rom_pgm_calls());
	ut_asserteq(0, *reg(SANDBOX_ADI_SC58X_RCU + RCU_CRCTL));

	/* A dry run still applies every rule */
	buf_init(&b);
	buf_word(&b, RESERVED, 1);
	ut_asserteq(-EACCES,
		    adi_otp_write_buffer(dev, buf_finish(&b, ADI_OTP_BUF_FLAG_DRY_RUN)));

	return 0;
}
DM_TEST(dm_test_adi_otp_dry_run, UTF_SCAN_FDT);

/* SC58x: nothing that could keep the part from booting */
static int dm_test_adi_otp_boot(struct unit_test_state *uts)
{
	struct udevice *dev;
	struct otp_buf b;

	ut_assertok(setup(uts, &dev));

	/* Booted in SPI master mode (1): it stays enabled */
	buf_init(&b);
	buf_word(&b, BMD, BMD_SPI_MASTER);
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	/* ... and so does its boot command (boot_info word 4) */
	buf_init(&b);
	buf_word(&b, BI(BI_BCMD_SPI_MASTER), BCMD);
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* The clocks: only the settings the running system uses */
	buf_init(&b);
	buf_word(&b, BI(BI_CGU0), BI_CGU0_CTL_WEN);
	buf_word(&b, BI(BI_CGU1), SANDBOX_ADI_SC58X_MSEL + 1);
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* The preboot DMC setup is never enabled (anomaly 20000073) */
	buf_init(&b);
	buf_word(&b, BI(BI_BCFG5), BI_BCFG5_DMC_EN);
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	ut_asserteq(0, sandbox_adi_rom_pgm_calls());

	/* What does not affect this boot mode, and the running clocks */
	buf_init(&b);
	buf_word(&b, BMD, BMD_SPI_SLAVE);
	buf_word(&b, BI(BI_BCMD_SPI_SLAVE), BCMD);
	buf_word(&b, BI(BI_CGU0), BI_CGU0_CTL_WEN);
	buf_word(&b, BI(BI_CGU1), SANDBOX_ADI_SC58X_MSEL);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* Without a known boot mode, no boot mode settings at all */
	*reg(SANDBOX_ADI_SC58X_RCU + RCU_STAT) = 0;
	buf_init(&b);
	buf_word(&b, BI(BI_BCMD_LINKPORT), BCMD);
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	return 0;
}
DM_TEST(dm_test_adi_otp_boot, UTF_SCAN_FDT);

/* SC58x: each boot-safety rule can be turned off by its flag, and only it */
static int dm_test_adi_otp_unchecked(struct unit_test_state *uts)
{
	const u32 boot_mode = ADI_OTP_SC58X_FLAG_BOOT_MODE_UNCHECKED;
	const u32 cgu = ADI_OTP_SC58X_FLAG_CGU_UNCHECKED;
	const u32 dmc = ADI_OTP_SC58X_FLAG_DMC_UNCHECKED;
	const u32 msel = SANDBOX_ADI_SC58X_MSEL + 1;
	struct otp_buf mode_buf, cgu_buf, dmc_buf;
	struct udevice *dev;
	u8 *otp;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	/* The mode the part boots in: its disable bit and boot command */
	buf_init(&mode_buf);
	buf_word(&mode_buf, BMD, BMD_SPI_MASTER);
	buf_word(&mode_buf, BI(BI_BCMD_SPI_MASTER), BCMD);
	/* Clock settings other than the running ones */
	buf_init(&cgu_buf);
	buf_word(&cgu_buf, BI(BI_CGU0), BI_CGU0_CTL_WEN);
	buf_word(&cgu_buf, BI(BI_CGU1), msel);
	/* The preboot DMC setup */
	buf_init(&dmc_buf);
	buf_word(&dmc_buf, BI(BI_BCFG5), BI_BCFG5_DMC_EN);

	/* Each flag turns off its own rule only */
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev,
						 buf_finish(&mode_buf, cgu | dmc)));
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev,
						 buf_finish(&cgu_buf, boot_mode | dmc)));
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev,
						 buf_finish(&dmc_buf, boot_mode | cgu)));
	ut_asserteq(0, sandbox_adi_rom_pgm_calls());

	/* ... and with it, the request is programmed */
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&mode_buf, boot_mode)));
	ut_asserteq(BMD_SPI_MASTER, otp[BMD]);
	ut_asserteq(BCMD, otp[BI(BI_BCMD_SPI_MASTER)]);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&cgu_buf, cgu)));
	ut_asserteq(msel, otp[BI(BI_CGU1)]);
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&dmc_buf, dmc)));
	ut_asserteq(BI_BCFG5_DMC_EN, otp[BI(BI_BCFG5)]);

	return 0;
}
DM_TEST(dm_test_adi_otp_unchecked, UTF_SCAN_FDT);

/* SC58x locking: its conditions, then no more programming */
static int dm_test_adi_otp_lock(struct unit_test_state *uts)
{
	const u32 lock_fsn_copy = ADI_OTP_BUF_FLAG_LOCK |
				  ADI_OTP_SC58X_FLAG_FSN_COPY;
	struct udevice *dev;
	struct otp_buf b;
	u32 val;
	u8 *otp;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	/* No key for secure boot */
	buf_init(&b);
	buf_data(&b, EMU_KEY, test_key, sizeof(test_key));
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, lock_fsn_copy)));

	/* No FSN in gp1 (anomaly 20000108) */
	buf_init(&b);
	buf_data(&b, PUBLIC_KEY0, test_key, sizeof(test_key));
	buf_data(&b, EMU_KEY, test_key, sizeof(test_key));
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev,
						 buf_finish(&b, ADI_OTP_BUF_FLAG_LOCK)));
	ut_asserteq(-EINVAL, adi_otp_write_buffer(dev,
						  buf_finish(&b, lock_fsn_copy |
							     ADI_OTP_SC58X_FLAG_FSN_IGNORE)));

	/* A blank debug key, only if asked for */
	buf_init(&b);
	buf_data(&b, PUBLIC_KEY0, test_key, sizeof(test_key));
	ut_asserteq(-EPERM, adi_otp_write_buffer(dev, buf_finish(&b, lock_fsn_copy)));
	ut_assertok(adi_otp_write_buffer(dev,
					 buf_finish(&b, lock_fsn_copy |
						    ADI_OTP_SC58X_FLAG_EMU_KEY_BLANK |
						    ADI_OTP_BUF_FLAG_DRY_RUN)));
	ut_asserteq(0, sandbox_adi_rom_pgm_calls());

	/* Key, debug key and FSN copy: locked, last */
	buf_init(&b);
	buf_data(&b, PUBLIC_KEY0, test_key, sizeof(test_key));
	buf_data(&b, EMU_KEY, test_key, sizeof(test_key));
	ut_assertok(adi_otp_write_buffer(dev, buf_finish(&b, lock_fsn_copy)));
	ut_asserteq_mem(test_key, otp + PUBLIC_KEY0, sizeof(test_key));
	ut_asserteq_mem(otp + FSN, otp + GP1, FSN_BYTES);
	ut_asserteq(LOCK_BIT, otp[LOCK] & LOCK_BIT);

	/* Pending lock: no more programming */
	ut_asserteq(-EROFS, adi_otp_program_word(dev, WORD(GP1) + 8, 1));

	/* After a reset: locked, and the serial number no longer reads */
	sandbox_adi_rom_reset();
	ut_asserteq(-EROFS, adi_otp_program_word(dev, WORD(GP1) + 8, 1));
	ut_assertok(adi_otp_read_word(dev, WORD(FSN), &val));
	ut_asserteq(0, val);

	return 0;
}
DM_TEST(dm_test_adi_otp_lock, UTF_SCAN_FDT);

/* FSN copy: into the first blank 16-byte slot of gp1, once */
static int dm_test_adi_otp_fsn_copy(struct unit_test_state *uts)
{
	struct udevice *dev;
	struct otp_buf b;
	unsigned int calls;
	u8 *otp;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	buf_init(&b);
	buf_word(&b, GP1, 0x1234);
	ut_assertok(adi_otp_write_buffer(dev,
					 buf_finish(&b, ADI_OTP_SC58X_FLAG_FSN_COPY)));
	/* The second slot: the first holds data */
	ut_asserteq_mem(otp + FSN, otp + GP1 + FSN_BYTES, FSN_BYTES);

	calls = sandbox_adi_rom_pgm_calls();
	buf_init(&b);
	ut_assertok(adi_otp_write_buffer(dev,
					 buf_finish(&b, ADI_OTP_SC58X_FLAG_FSN_COPY)));
	ut_asserteq(calls, sandbox_adi_rom_pgm_calls());

	return 0;
}
DM_TEST(dm_test_adi_otp_fsn_copy, UTF_SCAN_FDT);

/* Failures while programming are reported, never ignored */
static int dm_test_adi_otp_faults(struct unit_test_state *uts)
{
	struct udevice *dev;
	struct otp_buf b;
	unsigned int calls;
	u8 *otp;

	ut_assertok(setup(uts, &dev));
	otp = sandbox_adi_rom_otp();

	/*
	 * A half-word that does not take (anomaly 20000129): the field does
	 * not read back, and programming stops before the next field
	 */
	sandbox_adi_rom_inject(PUBLIC_KEY0 + 2, SANDBOX_ADI_ROM_OK);
	buf_init(&b);
	buf_data(&b, PUBLIC_KEY0, test_key, sizeof(test_key));
	buf_data(&b, GP1, test_key, sizeof(test_key));
	ut_asserteq(-EIO, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	ut_asserteq(0, otp[GP1]);
	sandbox_adi_rom_inject(-1, SANDBOX_ADI_ROM_OK);

	/* The ROM reporting a failure */
	sandbox_adi_rom_inject(-1, SANDBOX_ADI_ROM_FAIL);
	buf_init(&b);
	buf_word(&b, GP1, 1);
	ut_asserteq(-EIO, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* Success, but an OTPC error bit (anomaly 20000039) */
	sandbox_adi_rom_inject(-1, SANDBOX_ADI_ROM_STAT_ERROR);
	buf_init(&b);
	buf_word(&b, STAGE_ID, 1);
	ut_asserteq(-EIO, adi_otp_write_buffer(dev, buf_finish(&b, 0)));

	/* A busy PMC: the ROM would wait forever, so it is not called */
	calls = sandbox_adi_rom_pgm_calls();
	sandbox_adi_rom_inject(-1, SANDBOX_ADI_ROM_PMC_BUSY);
	buf_init(&b);
	buf_word(&b, HUK, 1);
	ut_asserteq(-EIO, adi_otp_write_buffer(dev, buf_finish(&b, 0)));
	ut_asserteq(calls, sandbox_adi_rom_pgm_calls());
	ut_asserteq(0, otp[HUK]);

	return 0;
}
DM_TEST(dm_test_adi_otp_faults, UTF_SCAN_FDT);

/* What the driver relies on in a SoC family's data */
static int check_soc(struct unit_test_state *uts,
		     const struct adi_otp_soc *soc)
{
	const struct adi_otp_soc_ops *ops = soc->ops;
	unsigned int i, end = 0;

	ut_assertf(soc->write_size == sizeof(u8) ||
		   soc->write_size == sizeof(u16) ||
		   soc->write_size == sizeof(u32),
		   "%s: write size %u\n", soc->name, soc->write_size);
	ut_assertf(!(soc->size % sizeof(u32)), "%s: size 0x%x\n", soc->name,
		   soc->size);
	ut_assertf(!(soc->flags & ~ADI_OTP_BUF_FLAGS_SOC),
		   "%s: flags 0x%x outside the SoC bits\n", soc->name,
		   soc->flags);

	/* The required operations */
	ut_assertnonnull(ops);
	ut_assertnonnull(ops->probe);
	ut_assertnonnull(ops->lock_state);
	ut_assertnonnull(ops->read);
	ut_assertnonnull(ops->read_counter);
	ut_assertnonnull(ops->apply);
	ut_assertnonnull(ops->lock);

	/* Fields: sorted, not overlapping, word aligned, in the area */
	for (i = 0; i < soc->nfields; i++) {
		const struct adi_otp_field *f = &soc->fields[i];

		ut_assertf(f->offset >= end, "%s overlaps the field before\n",
			   f->name);
		ut_assertf(!(f->offset % sizeof(u32)) && f->words,
			   "%s: offset 0x%x, %u words\n", f->name, f->offset,
			   f->words);
		ut_assertf(f->offset + adi_otp_field_bytes(f) <= soc->size,
			   "%s: beyond the area\n", f->name);
		if (f->kind == ADI_OTP_VALUE)
			ut_assertf(f->words == 1 && f->val_mask,
				   "%s: a value is one word, with a mask\n",
				   f->name);
		if (f->kind == ADI_OTP_COUNTER)
			ut_assertf(f->counter_max, "%s: no maximum\n",
				   f->name);
		end = f->offset + adi_otp_field_bytes(f);
	}

	/* Guards: whole write units, in the area */
	for (i = 0; i < soc->nguards; i++) {
		const struct adi_otp_guard *g = &soc->guards[i];

		ut_assertf(g->bytes && !(g->offset % soc->write_size) &&
			   !(g->bytes % soc->write_size) &&
			   g->offset + g->bytes <= soc->size,
			   "guard 0x%x+%u (%s)\n", g->offset, g->bytes,
			   g->reason);
	}

	return 0;
}

/* The SoC data of every ADI OTP device */
static int dm_test_adi_otp_soc_data(struct unit_test_state *uts)
{
	struct udevice *dev;
	unsigned int n;

	sandbox_set_enable_memio(true);
	sandbox_adi_rom_init();
	for (n = 0; !adi_otp_get_device(n, &dev); n++)
		ut_assertok(check_soc(uts, adi_otp_get_soc(dev)));
	ut_assert(n);

	return 0;
}
DM_TEST(dm_test_adi_otp_soc_data, UTF_SCAN_FDT);
