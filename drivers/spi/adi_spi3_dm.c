// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Converted to driver model by Nathan Barrett-Morrison
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 *
 */

#include <common.h>
#include <log.h>
#include <malloc.h>
#include <spi.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <dm.h>
#include <errno.h>
#include <fdtdec.h>
#include "adi_spi3_dm.h"
#include "spi-mem.h"
#include <clk.h>
#include <linux/iopoll.h>       // readl_poll_timeout
#include <asm/cache.h>          // invalidate_dcache_range

DECLARE_GLOBAL_DATA_PTR;

/* Basic functional description
 *
 * Note: Transfer flow has been ported from TF-A qspi driver
 *       Unlike TF-A driver, transaction is managed at a higher level (spi-mem.c).
 *
 * The functionality exposed to the user (adi_spi_xfer) does a single tx or rx
 * transfer (ie: send 'cmd+addr+dummy' bytes). This function calls
 * 'adi_spi_pio_xfer' which takes care of the HW transfer length limit (64 KB)
 * by spliting the transfer (if applicable).in several blocks.
 * Finally, there are two primitives (adi_qspi_tx_xfer, adi_qspi_rx_xfer)
 * intended to either send or receive data.
 *
 * Driver characteristics:
 *
 * - Support all SPI protocols (standard SPI, Extended, Dual and Quad)
 *   - Note: Extended Protocol 1-1-1 or 1-1-4 only (higher layer limitation)
 * - Chip select managed by FW
 * - Support DMA (DDE)
 * - Poll based design (blocking calls)
 * - SPI word transfer size is always 8-bit
 * - DMA transfer width as large as possible
 *
 * Not supported:
 * - 16-bit and 32-bit SPI transfer word size
 * - Some Extended Protocol options (ie: 1-4-4, 1-2-2, ...)
 *
 * Not tested:
 * - CS configured as GPIO
 * - MMAP
 */

#define DEBUG

#define MAX_SPI_NUM 2

#define QSPI_FIFO_TIMEOUT_US        5000U               /* Timeout for CMD, ADDR and dummy bytes (less than FIFO size) */
#define QSPI_DATA_TIMEOUT_US        15000000U           /* Timeout for data block (based on worst case SPI_MIN_FREQ and max transfer size(64KB) */

/* DDE Regs */
#define SPI_SIZE                    0U                  /* SPI word transfer size to 8-bit */
#define DDE_PSIZE                   0U                  /* Peripheral-DDE bus width to 8-bit */
#define DDE_MSIZE_MAX               5U                  /* DDE-System memory bus width range: 8-bit to 64-bit */
#define DDE_STAT_STOPPED            (DDE_STAT_RUN_STOPPED << DDE_STAT_RUN_OFFSET)

/* SPI Regs */
#define SPI_TXCTL_TDR_NOT_FULL      (SPI_TXCTL_TDR_NF)                  /* TDR: TFIFO not full */
#define SPI_RXCTL_RDR_NOT_EMPTY     (SPI_RXCTL_RDR_NE)                  /* RDR: RFIFO not empty */
#define MAX_TRANSFER_WORD_COUNT     ((SPI_TWC_VALUE / 32) * 32)         /* Max SPI block transfer size valid for all DDE configurations (multiple of 32 bytes)*/

int adi_spi_cs_valid(unsigned int bus, unsigned int cs)
{
	if (bus > MAX_SPI_NUM)
		return 0;
	return cs >= 1 && cs <= MAX_CTRL_CS;
}
/* TODO: gpio cs is not currently supported */
int cs_is_valid(unsigned int bus, unsigned int cs)
{
	if (is_gpio_cs(cs))
		return 0;
	else
		return adi_spi_cs_valid(bus, cs);
}

static int adi_spi_ofdata_to_platdata(struct udevice *bus)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	const void *blob = gd->fdt_blob;
	int node = dev_of_offset(bus);
	int subnode;
	fdt_addr_t addr;
	fdt_addr_t tx_dde_addr;
	fdt_addr_t rx_dde_addr;

	plat->bus_num = fdtdec_get_int(blob, node, "bus-num", 0);

	addr = dev_read_addr_index(bus, 0);
	if (FDT_ADDR_T_NONE == addr)
		return -EINVAL;

	plat->regs = (struct adi_spi_regs *)addr;

	/* DMA support (optional) */
	tx_dde_addr = dev_read_addr_index(bus, 1);
	rx_dde_addr = dev_read_addr_index(bus, 2);
	if ((FDT_ADDR_T_NONE == tx_dde_addr) || (FDT_ADDR_T_NONE == rx_dde_addr)) {
		/* No DMA support */
		plat->tx_dde_regs = NULL;
		plat->rx_dde_regs = NULL;
	} else {
		plat->tx_dde_regs = (struct adi_spi_dde_regs *)tx_dde_addr;
		plat->rx_dde_regs = (struct adi_spi_dde_regs *)rx_dde_addr;
	}

	/* All other paramters are embedded in the child node */
	subnode = fdt_first_subnode(blob, node);
	if (subnode < 0) {
		printf("Error: subnode with SPI flash config missing!\n");
		return -ENODEV;
	}
	plat->max_hz = fdtdec_get_int(blob, subnode, "spi-max-frequency", 500000);


	/* Read other parameters from DT */
	plat->cs_num = fdtdec_get_int(blob, subnode, "reg", 0);

	return 0;
}

static int adi_spi_probe(struct udevice *bus)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	struct adi_spi_priv *priv = dev_get_priv(bus);
	int cs;

	priv->bus_num = plat->bus_num;
	priv->cs_num = plat->cs_num;
	priv->regs = plat->regs;
	priv->tx_dde_regs = plat->tx_dde_regs;
	priv->rx_dde_regs = plat->rx_dde_regs;
	priv->use_dma = (plat->tx_dde_regs && plat->rx_dde_regs) ? true : false;

	if (!cs_is_valid(priv->bus_num, priv->cs_num)) {
		printf("Invalid chip select\r\n");
		return -ENODEV;
	}
	if (is_gpio_cs(priv->cs_num)) {
		cs = gpio_cs(priv->cs_num);
		gpio_request(cs, "adi-spi3");
		gpio_direction_output(cs, !priv->cs_pol);
	}

	writel(0x0, &priv->regs->control);
	writel(0x0, &priv->regs->rx_control);
	writel(0x0, &priv->regs->tx_control);

	if (priv->use_dma) {
		writel(0x0, &priv->tx_dde_regs->config);
		writel(0x0, &priv->rx_dde_regs->config);
		debug("qspi0: dma enabled\n");
	} else {
		debug("qspi0: dma disabled\n");
	}

	return 0;
}

static int adi_spi_remove(struct udevice *dev)
{
	return -ENODEV;
}

static int adi_spi_claim_bus(struct udevice *dev)
{
	struct adi_spi_priv *priv;
	struct udevice *bus = dev->parent;

	priv = dev_get_priv(bus);

	debug("%s: control:%i clock:%i\n", __func__, priv->control, priv->clock);

	/* SPI_CTL_MSTR bit can only be changed when SPI is disabled */
	writel((priv->control & ~SPI_CTL_EN), &priv->regs->control);
	writel(((priv->control & ~SPI_CTL_EN) | SPI_CTL_MSTR), &priv->regs->control);
	writel(priv->control, &priv->regs->control);

	writel(priv->clock, &priv->regs->clock);
	writel(0x0, &priv->regs->delay);

	return 0;
}

static int adi_spi_release_bus(struct udevice *dev)
{
	struct adi_spi_priv *priv;
	struct udevice *bus = dev->parent;

	priv = dev_get_priv(bus);

	debug("%s: control:%i clock:%i\n", __func__, priv->control, priv->clock);

	writel(0, &priv->regs->rx_control);
	writel(0, &priv->regs->tx_control);
	writel(0, &priv->regs->control);

	if (priv->use_dma) {
		u32 val;
		int ret;

		/* Disable DMA channels
		 * It is also a good practice to ensure that internal machinery
		 * is clean in case of coming from an error */
		writel(0, &priv->tx_dde_regs->config);
		ret = readl_poll_timeout(&priv->tx_dde_regs->status, val,
					 (val & DDE_STAT_RUN) == DDE_STAT_STOPPED,
					 QSPI_DATA_TIMEOUT_US);
		if (ret != 0)
			return ret;

		writel(0, &priv->rx_dde_regs->config);
		ret = readl_poll_timeout(&priv->rx_dde_regs->status, val,
					 (val & DDE_STAT_RUN) == DDE_STAT_STOPPED,
					 QSPI_DATA_TIMEOUT_US);
		if (ret != 0)
			return ret;
	}

	return 0;
}

void adi_spi_cs_activate(struct adi_spi_priv *priv)
{
	if (is_gpio_cs(priv->cs_num)) {
		unsigned int cs = gpio_cs(priv->cs_num);
		gpio_set_value(cs, priv->cs_pol);
	} else {
		u32 ssel;
		ssel = readl(&priv->regs->ssel);
		ssel |= BIT_SSEL_EN(priv->cs_num);
		if (priv->cs_pol)
			ssel |= BIT_SSEL_VAL(priv->cs_num);
		else
			ssel &= ~BIT_SSEL_VAL(priv->cs_num);
		writel(ssel, &priv->regs->ssel);
	}
}

void adi_spi_cs_deactivate(struct adi_spi_priv *priv)
{
	if (is_gpio_cs(priv->cs_num)) {
		unsigned int cs = gpio_cs(priv->cs_num);
		gpio_set_value(cs, !priv->cs_pol);
		gpio_set_value(cs, 1);
	} else {
		u32 ssel;
		ssel = readl(&priv->regs->ssel);
		if (priv->cs_pol)
			ssel &= ~BIT_SSEL_VAL(priv->cs_num);
		else
			ssel |= BIT_SSEL_VAL(priv->cs_num);
		/* deassert cs */
		writel(ssel, &priv->regs->ssel);
		/* disable cs */
		ssel &= ~BIT_SSEL_EN(priv->cs_num);
		writel(ssel, &priv->regs->ssel);
	}
}

static void adi_qspi_get_buses_size(uintptr_t addr, u32 len, u8 *psize, u8 *msize)
{
	int nbytes;
	int i;

	*msize = 0;
	*psize = DDE_PSIZE;
	for (i = DDE_MSIZE_MAX; i >= 0; i--) {
		nbytes = (1 << i);
		if (((addr % nbytes) == 0) && ((len % nbytes) == 0)) {
			*msize = i;
			break;
		}
	}
}

static int adi_qspi_tx_xfer(struct adi_spi_priv *priv, u8 *buf, u32 transfer_len)
{
	int ret = 0;
	int i;
	u32 val;
	u32 ctl;
	u8 psize;
	u8 msize = 0;

	/*  Clean status and tx_ctl */
	writel(0, &priv->regs->tx_control);
	writel(0xFFFFFFFF, &priv->regs->status);

	/* Set bytes to send.*/
	writel(transfer_len, &priv->regs->twc);
	writel(0, &priv->regs->twcr);

	/* Enable transmitter */
	ctl = SPI_TXCTL_TEN | SPI_TXCTL_TTI | SPI_TXCTL_TWCEN;
	if (priv->use_dma)
		ctl |= SPI_TXCTL_TDR_NOT_FULL;
	writel(ctl, &priv->regs->tx_control);

	/* Send */
	if (priv->use_dma) {
		uint32_t cfg;

		/* Get PSIZE/MSIZE */
		adi_qspi_get_buses_size((uintptr_t)buf, transfer_len, &psize, &msize);

		/* Clean and invalidate dcache before using DMA */
		flush_dcache_range((uintptr_t)buf, (uintptr_t)(buf + transfer_len));

		/* Configure Tx DDE */
		writel((uintptr_t)buf, &priv->tx_dde_regs->addr_start);
		writel(transfer_len >> msize, &priv->tx_dde_regs->x_count);
		writel(1 << msize, &priv->tx_dde_regs->x_modify);
		writel(0, &priv->tx_dde_regs->y_count);
		writel(0, &priv->tx_dde_regs->y_modify);

		cfg = (msize << DDE_CFG_MSIZE_OFFSET) |
		      (psize << DDE_CFG_PSIZE_OFFSET) |
		      DDE_CFG_SYNC | DDE_CFG_EN;
		writel(cfg, &priv->tx_dde_regs->config);

		/* Wait for DMA to complete */
		ret = readl_poll_timeout(&priv->tx_dde_regs->status, val,
					 (val & DDE_STAT_RUN) == DDE_STAT_STOPPED,
					 QSPI_DATA_TIMEOUT_US);
		if (ret != 0) {
			printf("%s: DDE tx timeout\n", __func__);
			goto tx_transfer_end;
		}
	} else {
		for (i = 0; i < transfer_len; i++) {
			ret = readl_poll_timeout(&priv->regs->status, val,
						 (val & SPI_STAT_TFF) != SPI_STAT_TFF,
						 QSPI_DATA_TIMEOUT_US);
			if (ret != 0) {
				printf("%s: tx fifo timeout\n", __func__);
				goto tx_transfer_end;
			}
			writel(*buf, &priv->regs->tfifo);
			buf++;
		}
	}

	/* Wait for transmission to complete */
	ret = readl_poll_timeout(&priv->regs->status, val,
				 (val & SPI_STAT_TF) == SPI_STAT_TF,
				 QSPI_DATA_TIMEOUT_US);
	if (ret != 0) {
		printf("%s: tx finish timeout\n", __func__);
		goto tx_transfer_end;
	}

tx_transfer_end:
	/* Clear SPI TXCTL and DDE CFG registers */
	if (priv->use_dma)
		writel(0, &priv->tx_dde_regs->config);
	writel(0, &priv->regs->tx_control);

	return ret;
}

static int adi_qspi_rx_xfer(struct adi_spi_priv *priv, u8 *buf, u32 transfer_len)
{
	int ret = 0;
	int len;
	u32 ctl;
	u32 val;
	u8 psize;
	u8 msize = 0;

	/*  Clean status and rx_ctl */
	writel(0, &priv->regs->rx_control);
	writel(0xFFFFFFFF, &priv->regs->status);

	/* Clean RFIFO (although it should be empty) */
	while (!(readl(&priv->regs->status) & SPI_STAT_RFE))
		readl(&priv->regs->rfifo);

	/* Set bytes to receive */
	writel(transfer_len, &priv->regs->rwc);
	writel(0, &priv->regs->rwcr);

	ctl = SPI_RXCTL_REN | SPI_RXCTL_RTI | SPI_RXCTL_RWCEN;

	/* Receive */
	if (priv->use_dma) {
		uint32_t cfg;

		/* Get PSIZE/MSIZE */
		adi_qspi_get_buses_size((uintptr_t)buf, transfer_len, &psize, &msize);

		/* Invalidate dcache before using DMA */
		invalidate_dcache_range((uintptr_t)buf, (uintptr_t)(buf + transfer_len));

		/* Configure Rx DDE */
		writel((uintptr_t)buf, &priv->rx_dde_regs->addr_start);
		writel(transfer_len >> msize, &priv->rx_dde_regs->x_count);
		writel(1 << msize, &priv->rx_dde_regs->x_modify);
		writel(0, &priv->rx_dde_regs->y_count);
		writel(0, &priv->rx_dde_regs->y_modify);

		cfg = (msize << DDE_CFG_MSIZE_OFFSET) |
		      (psize << DDE_CFG_PSIZE_OFFSET) |
		      DDE_CFG_WNR | DDE_CFG_SYNC | DDE_CFG_EN;
		writel(cfg, &priv->rx_dde_regs->config);

		/* Set SPI->DDE trigger */
		ctl |= SPI_RXCTL_RDR_NOT_EMPTY;

		/* Enable receiver */
		writel(ctl, &priv->regs->rx_control);

		/* Wait for DMA to complete */
		ret = readl_poll_timeout(&priv->rx_dde_regs->status, val,
					 (val & DDE_STAT_RUN) == DDE_STAT_STOPPED,
					 QSPI_DATA_TIMEOUT_US);
		if (ret != 0) {
			printf("%s: DDE rx timeout\n", __func__);
			goto rx_transfer_end;
		}
	} else {
		/* Enable receiver */
		writel(ctl, &priv->regs->rx_control);

		for (len = transfer_len; len != 0U; len--) {
			ret = readl_poll_timeout(&priv->regs->status, val,
						 (val & SPI_STAT_RFE) != SPI_STAT_RFE,
						 QSPI_DATA_TIMEOUT_US);
			if (ret != 0) {
				printf("%s: DDE rx timeout\n", __func__);
				goto rx_transfer_end;
			}

			*buf = readl(&priv->regs->rfifo);
			buf++;
		}
	}

rx_transfer_end:
	/* Clear SPI RXCTL and DDE CFG registers */
	if (priv->use_dma)
		writel(0, &priv->rx_dde_regs->config);
	writel(0, &priv->regs->rx_control);

	return ret;
}

static int adi_spi_pio_xfer(struct adi_spi_priv *priv, const u8 *tx, u8 *rx,
			    uint bytes, unsigned long flags)
{
	int ret = 0;
	u32 miom;
	u32 rem_len;
	u16 transfer_len;
	u8 *buf;
	enum spi_mem_data_dir dir = rx ? SPI_MEM_DATA_IN :
				    tx ? SPI_MEM_DATA_OUT : SPI_MEM_NO_DATA;

	if (bytes == 0)
		/* Nothing to do */
		return 0;

	if (dir == SPI_MEM_NO_DATA)
		/* Unexpected */
		return -1;

	/* Set bus width (DUAL support missing in spi framework) */
	if (flags & SPI_XFER_QUAD)
		miom = SPI_CTL_MIO_QUAD;
	else
		miom = SPI_CTL_MIO_DIS;

	writel((priv->control & ~SPI_CTL_SOSI) | miom, &priv->regs->control);

	/* Do transfer */
	buf = (uint8_t *)((dir == SPI_MEM_DATA_IN) ? rx : tx);
	rem_len = bytes;
	while (rem_len > 0U) {
		/* Transfer length limited to MAX_TRANSFER_WORD_COUNT bytes (< 64KB) */
		transfer_len = (rem_len > MAX_TRANSFER_WORD_COUNT) ?
			       MAX_TRANSFER_WORD_COUNT : rem_len;

		if (dir == SPI_MEM_DATA_IN)
			ret = adi_qspi_rx_xfer(priv, buf, transfer_len);
		else
			ret = adi_qspi_tx_xfer(priv, buf, transfer_len);

		if (ret != 0)
			break;

		/* Update remaining length */
		rem_len -= transfer_len;
		buf += transfer_len;
	}

	return ret;
}

static int adi_spi_xfer(struct udevice *dev, unsigned int bitlen,
			const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct adi_spi_priv *priv = dev_get_priv(bus);

	const u8 *tx = dout;
	u8 *rx = din;
	uint bytes = bitlen / 8;
	int ret = 0;

	debug("%s: bus_num:%i cs:%i bitlen:%i bytes:%i flags:%lx\n", __func__,
	      priv->bus_num, priv->cs_num, bitlen, bytes, flags);

	if (bitlen == 0)
		goto done;

	/* we can only do 8 bit transfers */
	if (bitlen % 8) {
		flags |= SPI_XFER_END;
		goto done;
	}

	/* The transaction (ie: read 10KB) is managed at higher level. Each one
	 * is made-up of several calls to this function, resulting in this flow:
	 * - assert CS, one o more transfers, de-assert CS
	 */
	if (flags & SPI_XFER_BEGIN)
		adi_spi_cs_activate(priv);

	ret = adi_spi_pio_xfer(priv, tx, rx, bytes, flags);

done:
	if (flags & SPI_XFER_END)
		adi_spi_cs_deactivate(priv);

	return ret;
}

static int adi_spi_set_speed(struct udevice *bus, uint speed)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	struct adi_spi_priv *priv = dev_get_priv(bus);
	int ret;
	u32 clock, spi_base_clk;
	struct clk spi_clk;

	ret = clk_get_by_name(bus, "spi", &spi_clk);
	if (ret < 0) {
		printf("Can't get SPI clk: %d\n", ret);
		return ret;
	}
	spi_base_clk = clk_get_rate(&spi_clk);

	if (speed > plat->max_hz)
		speed = plat->max_hz;

	if (speed > spi_base_clk)
		return -ENODEV;

	/* Baud rate formula from spec */
	clock = (spi_base_clk / speed) - 1;

	/* Ensure that configured speed is equal or lower than requested */
	if ((spi_base_clk % speed) != 0)
		clock++;

	priv->clock = clock;

	debug("%s: priv->clock: %x, speed: %x, get_spi_clk(): %x\n", __func__, clock, speed, spi_base_clk);

	return 0;
}

static int adi_spi_set_mode(struct udevice *bus, uint mode)
{
	struct adi_spi_priv *priv = dev_get_priv(bus);
	uint32_t reg;

	reg = SPI_CTL_EN | SPI_CTL_MSTR;
	if (mode & SPI_CPHA)
		reg |= SPI_CTL_CPHA;
	if (mode & SPI_CPOL)
		reg |= SPI_CTL_CPOL;
	if (mode & SPI_LSB_FIRST)
		reg |= SPI_CTL_LSBF;
	reg &= ~SPI_CTL_ASSEL;

	priv->control = reg;
	priv->cs_pol = mode & SPI_CS_HIGH ? 1 : 0;

	debug("%s: control=%d, cs_pol=%d\n", __func__, reg, priv->cs_pol);

	return 0;
}

static const struct dm_spi_ops adi_spi_ops = {
	.claim_bus	= adi_spi_claim_bus,
	.release_bus	= adi_spi_release_bus,
	.xfer		= adi_spi_xfer,
	.set_speed	= adi_spi_set_speed,
	.set_mode	= adi_spi_set_mode,
};

static const struct udevice_id adi_spi_ids[] = {
	{ .compatible = "adi,spi3" },
	{ }
};

U_BOOT_DRIVER(adi_spi3) = {
	.name		= "adi_spi3",
	.id		= UCLASS_SPI,
	.of_match	= adi_spi_ids,
	.ops		= &adi_spi_ops,
	.of_to_plat	= adi_spi_ofdata_to_platdata,
	.probe		= adi_spi_probe,
	.remove		= adi_spi_remove,
	.plat_auto	= sizeof(struct adi_spi_platdata),
	.priv_auto	= sizeof(struct adi_spi_priv),
	.per_child_auto = sizeof(struct spi_slave),
};
