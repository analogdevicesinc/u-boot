// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2015-2026 Analog Devices Inc.
 *
 * ADI hardware reference detection commands for PlutoSDR/M2K/SidekiqZ2.
 * Provides XADC voltage reading and board revision detection.
 */

#include <command.h>
#include <dm.h>
#include <env.h>
#include <spi.h>
#include <spi_flash.h>
#include <vsprintf.h>
#include <version_string.h>
#include "xadcps.h"

static int do_xadc(struct cmd_tbl *cmdtp, int flag, int argc,
		   char *const argv[])
{
	static XAdcPs XAdcInst;
	XAdcPs_Config *ConfigPtr;
	XAdcPs *XAdcInstPtr = &XAdcInst;
	u32 RawData, chan, cnt = 2;
	int val;
	char buf[16];

	ConfigPtr = XAdcPs_LookupConfig(0);
	if (!ConfigPtr)
		return -1;

	if (argc > 1)
		chan = simple_strtoul(argv[1], NULL, 10);
	else
		chan = XADCPS_CH_VPVN;

	do {
		XAdcPs_CfgInitialize(XAdcInstPtr, ConfigPtr,
				     ConfigPtr->BaseAddress);
		XAdcPs_SetSingleChParams(XAdcInstPtr, chan, 0, 0, 0);
		XAdcPs_SetSequencerMode(XAdcInstPtr, XADCPS_SEQ_MODE_SINGCHAN);
		RawData = XAdcPs_GetAdcData(XAdcInstPtr, chan);
	} while (cnt--);

	XAdcPs_SetPowerdownMode(XAdcInstPtr, XADCPS_PD_MODE_XADC);

	val = ((RawData >> 4) * 1000) / 0xFFF;

	snprintf(buf, sizeof(buf), "%d", val);
	printf("%s\n", buf);

	return val;
}

U_BOOT_CMD(
	xadc, CONFIG_SYS_MAXARGS, 1, do_xadc,
	"read xadc channel",
	""
);

static int do_env_version(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	env_set("uboot-version", version_string);
	return 0;
}

U_BOOT_CMD(
	envversion, 1, 1, do_env_version,
	"set env variable uboot-version",
	""
);

/*
 * Detect PlutoSDR Rev.A by reading SPI flash JEDEC ID directly.
 * Original check from old u-boot spi_flash.c:
 *   jedec == 0xBB19 && ext_jedec == 0x1000 && idcode[5] == 0
 *
 * The 6-byte READ_ID response for Rev.A: 20 BB 19 10 00 00
 * Non-Rev.A boards have idcode[5] != 0 or different jedec/ext_jedec.
 */
static int pluto_detect_revA(void)
{
	struct udevice *dev, *bus;
	struct spi_slave *slave;
	u8 idcode[6];
	u16 jedec, ext_jedec;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_SPI, 0, &bus);
	if (ret)
		return 0;

	ret = spi_flash_probe_bus_cs(0, 0, &dev);
	if (ret)
		return 0;

	slave = dev_get_parent_priv(dev);
	if (!slave)
		return 0;

	ret = spi_claim_bus(slave);
	if (ret)
		return 0;

	/* Send READ_ID (0x9F) and read 6 bytes */
	u8 cmd = 0x9F;

	ret = spi_xfer(slave, 8, &cmd, NULL, SPI_XFER_BEGIN);
	if (!ret)
		ret = spi_xfer(slave, 48, NULL, idcode, SPI_XFER_END);

	spi_release_bus(slave);

	if (ret)
		return 0;

	/* idcode[0] = mfr_id (0x20 = Micron) */
	jedec = (idcode[1] << 8) | idcode[2];
	ext_jedec = (idcode[3] << 8) | idcode[4];

	if (jedec == 0xBB19 && ext_jedec == 0x1000 && idcode[5] == 0x00)
		return 1;

	return 0;
}

static int do_adi_hw_version(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	int val, ret = 0, i;
	char buf[16];

	if (pluto_detect_revA()) {
		env_set("PlutoRevA", "1");
		env_set("fit_config", "config@0");
		return 0;
	}

	env_set("PlutoRevA", "");

	val = do_xadc(cmdtp, flag, 1, argv);

	for (i = 100; i <= 1000; i += 100) {
		if ((val >= (i - 50)) && (val < (i + 50))) {
			ret = i / 100;
			break;
		}
	}

	snprintf(buf, sizeof(buf), "config@%d", ret);
	env_set("fit_config", buf);

	return ret;
}

U_BOOT_CMD(
	adi_hwref, CONFIG_SYS_MAXARGS, 1, do_adi_hw_version,
	"determine pluto/m2k/sidekiqz2 hw revision",
	""
);

