// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 * Converted to driver model by Nathan Barrett-Morrison
 *
 *
 */

#include <clk.h>
#include <dm.h>
#include <mapmem.h>
#include <spi.h>
#include <spi-mem.h>
#include <dm/device_compat.h>
#include <linux/io.h>
#include <adi_spi3.h>

#define SPI_IDLE_VAL	0xff

#define MAX_CTRL_CS 7

struct adi_spi_platdata {
	u32 max_hz;
	u32 bus_num;
	struct adi_spi_regs __iomem *regs;
};

struct adi_spi_priv {
	u32 control;
	u32 clock;
	u32 bus_num;
	u32 max_cs;
	struct adi_spi_regs __iomem *regs;
};

/**
 * By convention, this driver uses the same CS numbering that is used with the SSEL bit
 * definitions (both here and in the TRM on which this is based), which are 1-indexed not
 * 0-indexed. The valid CS range is therefore [1,max_cs], in contrast with other drivers
 * where it is [0,max_cs-1].
 */
static int adi_spi_cs_info(struct udevice *bus, uint cs,
			   struct spi_cs_info *info)
{
	struct adi_spi_priv *priv = dev_get_priv(bus);

	if (cs == 0 || cs > priv->max_cs) {
		dev_err(bus, "invalid chipselect %u\n", cs);
		return -EINVAL;
	}

	return 0;
}

static int adi_spi_of_to_plat(struct udevice *bus)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	fdt_addr_t addr;

	plat->max_hz = dev_read_u32_default(bus, "spi-max-frequency", 500000);
	plat->bus_num = dev_read_u32_default(bus, "bus-num", 0);
	addr = dev_read_addr(bus);

	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	plat->regs = map_sysmem(addr, sizeof(*plat->regs));

	return 0;
}

static int adi_spi_probe(struct udevice *bus)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	struct adi_spi_priv *priv = dev_get_priv(bus);

	priv->bus_num = plat->bus_num;
	priv->regs = plat->regs;
	priv->max_cs = dev_read_u32_default(bus, "num-cs", MAX_CTRL_CS);

	iowrite32(0x0, &plat->regs->control);
	iowrite32(0x0, &plat->regs->rx_control);
	iowrite32(0x0, &plat->regs->tx_control);

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

	debug("%s: control:%i clock:%i\n",
	      __func__, priv->control, priv->clock);

	iowrite32(priv->control, &priv->regs->control);
	iowrite32(priv->clock, &priv->regs->clock);
	iowrite32(0x0, &priv->regs->delay);

	return 0;
}

static int adi_spi_release_bus(struct udevice *dev)
{
	struct adi_spi_priv *priv;
	struct udevice *bus = dev->parent;

	priv = dev_get_priv(bus);

	debug("%s: control:%i clock:%i\n",
	      __func__, priv->control, priv->clock);

	iowrite32(0x0, &priv->regs->rx_control);
	iowrite32(0x0, &priv->regs->tx_control);
	iowrite32(0x0, &priv->regs->control);

	return 0;
}

void adi_spi_enable_ssel(struct adi_spi_priv *priv, int cs)
{
	setbits_32(&priv->regs->ssel, BIT_SSEL_EN(cs));
}

void adi_spi_set_ssel(struct adi_spi_priv *priv, int cs, int high)
{
	if (high)
		setbits_32(&priv->regs->ssel, BIT_SSEL_VAL(cs));
	else
		clrbits_32(&priv->regs->ssel, BIT_SSEL_VAL(cs));
}

void adi_spi_cs_activate(struct adi_spi_priv *priv, struct dm_spi_slave_plat *slave_plat)
{
	bool high = slave_plat->mode & SPI_CS_HIGH;

	adi_spi_set_ssel(priv, slave_plat->cs[0], high);
	adi_spi_enable_ssel(priv, slave_plat->cs[0]);
}

void adi_spi_cs_deactivate(struct adi_spi_priv *priv, struct dm_spi_slave_plat *slave_plat)
{
	bool high = slave_plat->mode & SPI_CS_HIGH;

	/* invert CS for matching SSEL to deactivate */
	adi_spi_set_ssel(priv, slave_plat->cs[0], !high);
}

static void discard_rx_fifo_contents(struct adi_spi_regs *regs)
{
	while (!(ioread32(&regs->status) & SPI_STAT_RFE))
		ioread32(&regs->rfifo);
}

static int adi_spi_fifo_mio_xfer(struct adi_spi_priv *priv, const u8 *tx, u8 *rx,
				 uint bytes, uint32_t mio_mode)
{
	u8 value;

	/* switch current SPI transfer to mio SPI mode */
	clrsetbits_32(&priv->regs->control, SPI_CTL_SOSI, mio_mode);
	/*
	 * Data can only be transferred in one direction in multi-io SPI
	 * modes, trigger the transfer in respective direction.
	 */
	if (rx) {
		iowrite32(0x0, &priv->regs->tx_control);
		iowrite32(SPI_RXCTL_REN | SPI_RXCTL_RTI, &priv->regs->rx_control);

		while (bytes--) {
			while (ioread32(&priv->regs->status) &
				SPI_STAT_RFE)
				if (ctrlc())
					return -1;
			value = ioread32(&priv->regs->rfifo);
			*rx++ = value;
		}
	} else if (tx) {
		iowrite32(0x0, &priv->regs->rx_control);
		iowrite32(SPI_TXCTL_TEN | SPI_TXCTL_TTI, &priv->regs->tx_control);

		while (bytes--) {
			value = *tx++;
			iowrite32(value, &priv->regs->tfifo);
			while (ioread32(&priv->regs->status) &
				SPI_STAT_TFF)
				if (ctrlc())
					return -1;
		}

		/* Wait till the tfifo is empty */
		while ((ioread32(&priv->regs->status) & SPI_STAT_TFS) != SPI_STAT_TFIFO_EMPTY)
			if (ctrlc())
				return -1;
	} else {
		return -1;
	}
	return 0;
}

static int adi_spi_fifo_1x_xfer(struct adi_spi_priv *priv, const u8 *tx, u8 *rx,
				uint bytes)
{
	u8 value;

	/*
	 * Set current SPI transfer in normal mode and trigger
	 * the bi-direction transfer by tx write operation.
	 */
	iowrite32(priv->control, &priv->regs->control);
	iowrite32(SPI_RXCTL_REN, &priv->regs->rx_control);
	iowrite32(SPI_TXCTL_TEN | SPI_TXCTL_TTI, &priv->regs->tx_control);

	while (bytes--) {
		value = (tx ? *tx++ : SPI_IDLE_VAL);
		debug("%s: tx:%x ", __func__, value);
		iowrite32(value, &priv->regs->tfifo);
		while (ioread32(&priv->regs->status) & SPI_STAT_RFE)
			if (ctrlc())
				return -1;
		value = ioread32(&priv->regs->rfifo);
		if (rx)
			*rx++ = value;
		debug("rx:%x\n", value);
	}
	return 0;
}

static int adi_spi_fifo_xfer(struct adi_spi_priv *priv, int buswidth,
			     const u8 *tx, u8 *rx, uint bytes)
{
	switch (buswidth) {
	case 1:
		return adi_spi_fifo_1x_xfer(priv, tx, rx, bytes);
	case 2:
		return adi_spi_fifo_mio_xfer(priv, tx, rx, bytes, SPI_CTL_MIO_DUAL);
	case 4:
		return adi_spi_fifo_mio_xfer(priv, tx, rx, bytes, SPI_CTL_MIO_QUAD);
	default:
		return -ENOTSUPP;
	}
}

static int adi_spi_xfer(struct udevice *dev, unsigned int bitlen,
			const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct adi_spi_priv *priv = dev_get_priv(bus);
	struct dm_spi_slave_plat *slave_plat = dev_get_parent_plat(dev);

	const u8 *tx = dout;
	u8 *rx = din;
	uint bytes = bitlen / 8;
	int ret = 0;

	debug("%s: bus_num:%i cs:%i bitlen:%i bytes:%i flags:%lx\n", __func__,
	      priv->bus_num, slave_plat->cs[0], bitlen, bytes, flags);

	if (flags & SPI_XFER_BEGIN)
		adi_spi_cs_activate(priv, slave_plat);

	if (bitlen == 0)
		goto done;

	/* we can only do 8 bit transfers */
	if (bitlen % 8) {
		flags |= SPI_XFER_END;
		goto done;
	}

	/* Discard invalid rx data and empty rfifo */
	discard_rx_fifo_contents(priv->regs);

	ret = adi_spi_fifo_1x_xfer(priv, tx, rx, bytes);

 done:
	if (flags & SPI_XFER_END)
		adi_spi_cs_deactivate(priv, slave_plat);

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
		dev_err(bus, "Can't get SPI clk: %d\n", ret);
		return ret;
	}
	spi_base_clk = clk_get_rate(&spi_clk);

	if (speed > plat->max_hz)
		speed = plat->max_hz;

	if (speed > spi_base_clk)
		return -ENODEV;

	clock = spi_base_clk / speed;
	if (clock)
		clock--;

	priv->clock = clock;

	debug("%s: priv->clock: %x, speed: %x, get_spi_clk(): %x\n",
	      __func__, clock, speed, spi_base_clk);

	return 0;
}

static int adi_spi_set_mode(struct udevice *bus, uint mode)
{
	struct adi_spi_priv *priv = dev_get_priv(bus);
	u32 reg;

	reg = SPI_CTL_EN | SPI_CTL_MSTR;
	if (mode & SPI_CPHA)
		reg |= SPI_CTL_CPHA;
	if (mode & SPI_CPOL)
		reg |= SPI_CTL_CPOL;
	if (mode & SPI_LSB_FIRST)
		reg |= SPI_CTL_LSBF;
	reg &= ~SPI_CTL_ASSEL;

	priv->control = reg;

	debug("%s: control=%d, cs_pol=%d\n", __func__, reg, mode & SPI_CS_HIGH ? 1 : 0);

	return 0;
}

/**
 * U-boot's version of spi-mem does not support mixed bus-width
 * commands nor anything more than 1x mode.
 * Using a custom exec_op implementation, we can support it.
 */
static int adi_spi_mem_exec_op(struct spi_slave *slave,
			       const struct spi_mem_op *op)
{
	int rv = 0;
	struct udevice *bus = slave->dev->parent;
	struct adi_spi_priv *priv = dev_get_priv(bus);
	struct dm_spi_slave_plat *slave_plat = dev_get_parent_plat(slave->dev);
	u8 tmpbuf[64];
	int i;

	if ((op->cmd.nbytes + op->addr.nbytes + op->dummy.nbytes) >
	    sizeof(tmpbuf))
		return -ENOMEM;

	for (i = 0; i < op->cmd.nbytes; i++)
		tmpbuf[i] = op->cmd.opcode >>
				(8 * (op->cmd.nbytes - i - 1));
	for (i = 0; i < op->addr.nbytes; i++)
		tmpbuf[i + op->cmd.nbytes] = op->addr.val >>
				(8 * (op->addr.nbytes - i - 1));
	memset(tmpbuf + op->addr.nbytes + op->cmd.nbytes, 0xff,
	       op->dummy.nbytes);

	adi_spi_cs_activate(priv, slave_plat);
	discard_rx_fifo_contents(priv->regs);

	if (op->cmd.nbytes) {
		rv = adi_spi_fifo_xfer(priv, op->cmd.buswidth,
				       tmpbuf, NULL, op->cmd.nbytes);
		if (rv != 0)
			goto cleanup;
	}

	if (op->addr.nbytes) {
		rv = adi_spi_fifo_xfer(priv, op->addr.buswidth,
				       tmpbuf + op->cmd.nbytes, NULL,
				       op->addr.nbytes);
		if (rv != 0)
			goto cleanup;
	}

	if (op->dummy.nbytes) {
		rv = adi_spi_fifo_xfer(priv, op->dummy.buswidth,
				       tmpbuf + op->cmd.nbytes +
				       op->addr.nbytes,
				       NULL, op->dummy.nbytes);
		if (rv != 0)
			goto cleanup;
	}

	if (op->data.dir == SPI_MEM_DATA_IN)
		rv = adi_spi_fifo_xfer(priv, op->data.buswidth,
				       NULL, op->data.buf.in,
				       op->data.nbytes);
	else if (op->data.dir == SPI_MEM_DATA_OUT)
		rv = adi_spi_fifo_xfer(priv, op->data.buswidth,
				       op->data.buf.out, NULL,
				       op->data.nbytes);

cleanup:
	adi_spi_cs_deactivate(priv, slave_plat);
	return rv;
}

static const struct spi_controller_mem_ops adi_spi_mem_ops = {
	.exec_op = adi_spi_mem_exec_op,
};

static const struct dm_spi_ops adi_spi_ops = {
	.claim_bus = adi_spi_claim_bus,
	.release_bus = adi_spi_release_bus,
	.xfer = adi_spi_xfer,
	.set_speed = adi_spi_set_speed,
	.set_mode = adi_spi_set_mode,
	.cs_info = adi_spi_cs_info,
	.mem_ops = &adi_spi_mem_ops,
};

static const struct udevice_id adi_spi_ids[] = {
	{ .compatible = "adi,spi3" },
	{ }
};

U_BOOT_DRIVER(adi_spi3) = {
	.name = "adi_spi3",
	.id = UCLASS_SPI,
	.of_match = adi_spi_ids,
	.ops = &adi_spi_ops,
	.of_to_plat = adi_spi_of_to_plat,
	.probe = adi_spi_probe,
	.remove = adi_spi_remove,
	.plat_auto = sizeof(struct adi_spi_platdata),
	.priv_auto = sizeof(struct adi_spi_priv),
	.per_child_auto = sizeof(struct spi_slave),
};
