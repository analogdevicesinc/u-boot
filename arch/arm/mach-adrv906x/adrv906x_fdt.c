/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#include <asm/global_data.h>
#include <asm/armv8/mmu.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <log.h>
#include <net.h>
#include <stdio.h>

#include <adrv906x.h>
#include <adrv906x_def.h>
#include <adrv906x_fdt.h>
#include <adrv_common.h>
#include <err.h>

#define MAX_BOOT_DEV_NAME_LENGTH    16
#define MAX_NUM_MACS                6
#define ETH_ADDR_LEN                6

DECLARE_GLOBAL_DATA_PTR;

int get_dual_tile(uint32_t *dual_tile)
{
	int node;

	node = fdt_path_offset(gd->fdt_blob, "/boot");
	if (node < 0)
		return -1;

	*dual_tile = fdtdec_get_uint(gd->fdt_blob, node, "dual-tile", 0);

	return 0;
}

int get_secondary_linux_enabled(uint32_t *secondary_linux_enabled)
{
	int node;
	uint32_t dual_tile = 0;

	node = get_dual_tile(&dual_tile);
	if (node != 0)
		return -1;

	/* Secondary linux is only enabled if dual-tile is also enabled */
	if (dual_tile == 1) {
		node = fdt_path_offset(gd->fdt_blob, "/boot");
		if (node < 0)
			return -1;

		*secondary_linux_enabled = fdtdec_get_uint(gd->fdt_blob, node, "secondary-linux-enabled", 0);
	} else {
		*secondary_linux_enabled = 0;
	}

	return 0;
}

int get_sysclk_freq(void)
{
	int node;
	uint32_t sysclk_freq;

	node = fdt_path_offset(gd->fdt_blob, "/sysclk");
	if (node < 0)
		return -1;

	sysclk_freq = fdtdec_get_uint(gd->fdt_blob, node, "clock-frequency", -1);

	return sysclk_freq;
}

int get_hsdigclk_freq(void)
{
	int node;
	uint32_t hsdigclk_freq;

	node = fdt_path_offset(gd->fdt_blob, "/hsdigclk");
	if (node < 0)
		return -1;

	hsdigclk_freq = fdtdec_get_uint(gd->fdt_blob, node, "clock-frequency", -1);

	return hsdigclk_freq;
}

static int get_emmcclk_freq(void)
{
	int node;

	node = fdt_path_offset(gd->fdt_blob, "/mmcclk");
	if (node < 0)
		return -1;

	return fdtdec_get_uint(gd->fdt_blob, node, "clock-frequency", -1);
}

bool is_sysc(void)
{
	char *plat = get_platform();

	if (!strncmp(plat, "sysc", 4))
		return true;
	else
		return false;
}

bool is_protium(void)
{
	char *plat = get_platform();

	if (!strncmp(plat, "protium", 7))
		return true;
	else
		return false;
}

bool is_palladium(void)
{
	char *plat = get_platform();

	if (!strncmp(plat, "palladium", 9))
		return true;
	else
		return false;
}

bool is_boot_device_active(const char *boot_device)
{
	const char *cur_boot_device = NULL;
	int ret;

	ret = get_boot_device(&cur_boot_device);
	if (ret == 0)
		if (strncmp(cur_boot_device, boot_device, MAX_BOOT_DEV_NAME_LENGTH) == 0)
			return true;

	return false;
}

static int plat_set_prop_okay(void *blob, char *node_name)
{
	int node = -1;
	int ret = -1;

	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Missing %s node in device tree", node_name);
		return -1;
	}
	ret = fdt_setprop_string(blob, node, "status", "okay");
	if (ret < 0) {
		plat_error_message("Unable to set status prop for node %s", node_name);
		return -1;
	}

	return 0;
}

static int plat_set_prop_disabled(void *blob, char *node_name)
{
	int node = -1;
	int ret = -1;

	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Missing %s node in device tree", node_name);
		return -1;
	}
	ret = fdt_setprop_string(blob, node, "status", "disabled");
	if (ret < 0) {
		plat_error_message("Unable to set status prop for node %s", node_name);
		return -1;
	}

	return 0;
}

static int plat_del_prop(void *blob, char *node_name, char *property)
{
	int node = -1;
	int ret = -1;

	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Missing %s node in device tree", node_name);
		return -1;
	}
	ret = fdt_delprop(blob, node, property);
	if (ret < 0) {
		plat_error_message("Unable to remove property %s for node %s", property, node_name);
		return -1;
	}

	return 0;
}

static int get_mac_address(uint8_t index, uint8_t **mac)
{
	int node;
	char name[MAX_NODE_NAME_LENGTH];
	char *node_name;

	if (index >= MAX_NUM_MACS)
		return -1;

	/* Get U-Boot eth node name (emacx) */
	if (index == 0) snprintf(name, MAX_NODE_NAME_LENGTH, "/ethernet@%08x", EMAC_1G_BASE);
	else snprintf(name, MAX_NODE_NAME_LENGTH, "/emac@%d", index + 1);
	node_name = name;

	/* There is a separate node for SystemC 1G ethernet (different IP and driver) */
	if (is_sysc() == true)
		if (index == 0) node_name = "/ethernet@2";

	node = fdt_path_offset(gd->fdt_blob, node_name);
	if (node < 0)
		return -1;

	*mac = (u8 *)fdt_getprop(gd->fdt_blob, node, "mac-address", NULL);
	if (*mac == NULL)
		return -1;

	return 0;
}

static int plat_get_reserved_region_size(void *blob, char *name, uint64_t *size)
{
	char node_name[MAX_NODE_NAME_LENGTH];
	int node;
	int address_cells;
	int size_cells;
	int len;
	uint32_t u32_size;
	uint64_t u64_size;
	u8 tmp[16]; /* Up to 64-bit address + 64-bit size */
	u8 *p = tmp;

	/* Get the number of address and size cells */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/reserved-memory");
	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Unable to find %s node", node_name);
		return -1;
	}

	address_cells = fdt_address_cells(blob, node);
	size_cells = fdt_size_cells(blob, node);

	/* Get the region size */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/reserved-memory/%s", name);
	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Unable to find %s node", node_name);
		return -1;
	}

	/* Limited 'libfdt' functionality for properties not being 32-bit
	 * Need to manually build the 'size' value from reg property
	 */
	p = (u8 *)fdt_getprop(blob, node, "reg", &len);
	if (p == 0)
		return -1;

	p += 4 * address_cells;

	if (size_cells == 2) {
		u64_size = *(uint64_t *)p;
		*size = fdt64_to_cpu(u64_size);
	} else {
		u32_size = *(uint32_t *)p;
		*size = fdt32_to_cpu(u32_size);
	}

	return 0;
}

static int plat_populate_reserved_region(void *blob, char *name, uint64_t address, uint64_t size)
{
	char node_name[MAX_NODE_NAME_LENGTH];
	int node;
	int address_cells;
	int size_cells;
	int ret;
	int len;
	u8 tmp[16]; /* Up to 64-bit address + 64-bit size */
	u8 *p = tmp;

	/* Get the number of address and size cells */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/reserved-memory");
	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Unable to find %s node", node_name);
		return -1;
	}

	address_cells = fdt_address_cells(blob, node);
	size_cells = fdt_size_cells(blob, node);

	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/reserved-memory/%s", name);
	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Unable to find %s node", node_name);
		return -1;
	}

	/* Limited 'libfdt' functionality for properties not being 32-bit
	 * Need to manually build the 'reg' (address + size) property value
	 */
	if (address_cells == 2)
		*(fdt64_t *)p = cpu_to_fdt64(address);
	else
		*(fdt32_t *)p = cpu_to_fdt32(address);
	p += 4 * address_cells;

	if (size_cells == 2)
		*(fdt64_t *)p = cpu_to_fdt64(size);
	else
		*(fdt32_t *)p = cpu_to_fdt32(size);
	p += 4 * size_cells;

	len = p - tmp;

	ret = fdt_setprop(blob, node, "reg", tmp, len);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Reserved memory regions are intended to be exclusively used by User space
 *
 * This is configured through device tree by including these nodes:
 * - /reserved-memory/sram-reserved@x to prevent Linux kernel using this L4 region
 * - /reserved-memory/ddr-reserved@x to prevent Linux kernel using this DDR region
 * - /sram-mmap@x to Link reserved L4 region to User space
 * - /ddr-mmap@x to Link reserved DDR region to User space
 *
 * Note: If the secondary is not running Linux (dual-tile), primary will take
 *       over the secondary memories.
 * Note: In addtion to /memory node (dedicated to DDR), there must be a
 *       /sram_memory node (dedicated to L4) to indicate Linux the available
 *       memory in the system
 *
 * Design:
 * - Device tree (DTS) is common for all use cases and must include all
 *   potentially required memory regions:
 *   - Primary: nodes for primary and secondary L4 and DDR memories
 *   - Secondary: only nodes for secondary L4 and DDR
 * - Device tree must use the above node names
 * - U-boot will fix up device tree (DTB) on the fly according to the use case
 *   (ie. populate DDR base address, disable secondary nodes when appropriate,
 *   ...)
 *   - Resulting primary DTB will still contain all nodes ending with 2, but
 *     they will be enabled or disabled (status property) according to the use
 *     case
 *
 * Use cases (and how the final DTB should look like):
 *   Single-tile:
 *        Primary DTB: reserve two L4 and one DDR regions from primary tile
 *
 *   Dual-tile, Linux secondary, DDR secondary
 *        Primary DTB:   reserve two L4 and one DDR regions from primary tile
 *        Secondary DTB: reserve one L4 and one DDR regions from secondary tile
 *
 *   Dual-tile, Linux secondary, NO DDR secondary
 *        Primary DTB:   reserve two L4 and one DDR regions from primary tile
 *        Secondary DTB: reserve one L4 region from secondary tile and one DDR
 *                       region from primary tile (via C2C)
 *
 *   Dual-tile, NO Linux secondary, DDR secondary
 *        Primary DTB:   reserve two L4 and one DDR regions from primary tile
 *                       reserve one L4 and one DDR regions from secondary tile
 *                       (via C2C)
 *        Secondary DTB: N/A
 *
 *   Dual-tile, NO Linux secondary, NO DDR secondary
 *        Primary DTB:   reserve two L4 and one DDR regions from primary tile
 *                       reserve one L4 region from secondary tile (via C2C)
 *        Secondary DTB: N/A
 *
 * This function does only fix up primary device tree. See
 * arch_misc_init() for the fix up on secondary DTB (based on fdt commands)
 */
static int plat_memory_regions_fixup(void *blob)
{
	int node;
	int ret;
	int len;
	char node_name[MAX_NODE_NAME_LENGTH];
	uint8_t *reg_tmp;
	uint32_t is_dual_tile;
	uint32_t is_secondary_linux_enabled;
	uint64_t address;
	uint64_t size;

	ret = get_dual_tile(&is_dual_tile);
	if (ret < 0)
		return ret;

	ret = get_secondary_linux_enabled(&is_secondary_linux_enabled);
	if (ret < 0)
		return ret;

	/* 1. Get region size from device tree */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "ddr-reserved@0");
	ret = plat_get_reserved_region_size(blob, node_name, &size);
	if (ret < 0)
		return ret;
	if (is_sysc() && (size > MAX_RESERVED_MEM_SIZE_SYSC))
		size = MAX_RESERVED_MEM_SIZE_SYSC;

	/* 2. DDR base address configured at the top of DDR seen by Linux */
	if (size >= gd->ram_size) {
		plat_error_message("ddr-reserved@0 region size is larger than DRAM size");
		return -1;
	}
	address = gd->ram_base + gd->ram_size - size;
	if (address < gd->ram_top) {
		plat_error_message("ddr-reserved@0 region size exceeds reserved region size");
		return -1;
	}

	/* Populate the node and enable it */
	ret = plat_populate_reserved_region(blob, node_name, address, size);
	if (ret < 0)
		return ret;
	plat_set_prop_okay(blob, "/reserved-memory/ddr-reserved@0");

	/* Optionally populate secondary regions
	 * - Use case: dual-tile with no Linux running on secondary
	 */
	if (is_dual_tile && !is_secondary_linux_enabled) {
		/* Enable secondary L4 reserved region */
		ret = plat_set_prop_okay(blob, "/reserved-memory/sram-reserved@2");
		if (ret < 0)
			return -1;

		/* and make it available to user space */
		ret = plat_set_prop_okay(blob, "/sram-mmap@2");
		if (ret < 0)
			return -1;

		if (mem_map[1].size != 0) {
			/* Add secondary DDR region */
			node = fdt_path_offset(blob, "/memory");
			if (node < 0) {
				plat_error_message("Unable to find reserved-memory node");
				return -1;
			}

			address = mem_map[1].phys;
			size = mem_map[1].size;

			if (fdt_appendprop_u32(blob, node, "reg", address) < 0)
				return -1;

			if (fdt_appendprop_u32(blob, node, "reg", size) < 0)
				return -1;

			/* Reserve that region (prevent Linux kernel to use it)
			 * - populate region and enable it
			 */
			snprintf(node_name, MAX_NODE_NAME_LENGTH, "ddr-reserved@1");
			ret = plat_populate_reserved_region(blob, node_name, address, size);
			if (ret < 0)
				return ret;

			ret = plat_set_prop_okay(blob, "/reserved-memory/ddr-reserved@1");
			if (ret < 0)
				return -1;

			/* and make it available to user space through custom driver */
			ret = plat_set_prop_okay(blob, "/ddr-mmap@1");
			if (ret < 0)
				return -1;
		}
	} else {
		/* Primary must not access secondary regions in the regular use
		 * case. Device-tree explicitly defines secondary L4 region, so
		 * it must be removed
		 */
		node = fdt_path_offset(blob, "/sram_memory");
		if (node < 0) {
			plat_error_message("Unable to find reserved-memory node");
			return -1;
		}

		/* Read primary and secondary L4 ranges */
		reg_tmp = (uint8_t *)fdt_getprop(blob, node, "reg", &len);
		if (reg_tmp == 0)
			return -1;

		/* Write back only the primary region */
		ret = fdt_setprop(blob, node, "reg", reg_tmp, len / 2);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int plat_eth_fixup(void *blob)
{
	int ret;
	uint32_t is_dual_tile;
	uint8_t *mac;
	int node;
	char name[MAX_NODE_NAME_LENGTH];
	char *node_name;

	ret = get_dual_tile(&is_dual_tile);
	if (ret < 0)
		return ret;

	for (int i = 0; i < MAX_NUM_MACS; i++) {
		if ((!is_dual_tile) && (i >= (MAX_NUM_MACS / 2)))
			/* Skip secondary MACs in single-tile */
			break;

		/* Get Linux eth node name */

		/* 1G eth */
		if ((i == 0) || (i == 3))
			snprintf(name, MAX_NODE_NAME_LENGTH, "/ethernet@%08X", (i == 0) ? EMAC_1G_BASE : SEC_EMAC_1G_BASE);
		/* 10/25G eth */
		else
			snprintf(name, MAX_NODE_NAME_LENGTH, "/adrv906x_net@%08X/ethernet-ports/port@%d",
				 (i < (MAX_NUM_MACS / 2)) ? EMAC_CMN_BASE : SEC_EMAC_CMN_BASE,
				 ((i == 1) || (i == 4)) ? 0 : 1);

		node_name = name;

		/* There is a separate node for SystemC 1G ethernet (different IP and driver) */
		if (is_sysc() == true)
			if (i == 0) node_name = "/ethernet@2";

		/* MAC should be propagated only if the corresponding Linux eth
		 * node is present, and its MAC value is undefined
		 */
		node = fdt_path_offset(blob, node_name);
		if ((node < 0) ||
		    (NULL != fdt_getprop(blob, node, "mac-address", NULL)))
			continue;

		/* Propagate mac information, if available, from u-boot to Linux dtb */
		if ((0 == get_mac_address(i, &mac)) && !is_zero_ethaddr(mac))
			ret = fdt_setprop(blob, node, "mac-address", mac, ETH_ADDR_LEN);
	}

	return ret;
}

static int plat_uio_fixup(void *blob)
{
	int ret;
	uint32_t is_dual_tile;
	uint32_t is_secondary_linux_enabled;

	ret = get_dual_tile(&is_dual_tile);
	if (ret < 0)
		return ret;

	ret = get_secondary_linux_enabled(&is_secondary_linux_enabled);
	if (ret < 0)
		return ret;

	if (is_dual_tile) {
		int parent;
		int node;
		const char *name;

		parent = fdt_path_offset(blob, "/");
		if (parent < 0)
			return FDT_ERR_NOTFOUND;

		fdt_for_each_subnode(node, blob, parent) {
			name = fdt_get_name(blob, node, NULL);
			if (!name)
				return FDT_ERR_NOTFOUND;

			/* Enable secondary regions */
			if ((!strncmp(name, "uio-adrv906x-regmap-sec-", 22)) ||
			    /* Enable c2c interrupts if linux is not running in the secondary */
			    ((!is_secondary_linux_enabled) &&
			     (!strncmp(name, "uio-adrv906x-c2c-interrupt-", 25)))) {
				if (fdt_getprop(blob, node, "status", NULL)) {
					ret = fdt_setprop_string(blob, node, "status", "okay");
					if (ret < 0) {
						plat_error_message("Unable to set status prop for node %s", name);
						return ret;
					}
				}
			}
		}
	}

	return 0;
}

/* TODO: Consider removing if/when SystemC support is removed */
static int plat_sysc_fixup(void *blob)
{
	int ret;
	int node;
	char node_name[MAX_NODE_NAME_LENGTH];
	uint32_t is_dual_tile;
	uint32_t is_secondary_linux_enabled;

	/* Enable SystemC-specific devices in kernel device tree */

	/* Disable non-SystemC eMMC, SD, and ethernet */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mmc@%08x", SD_0_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mmc@%08x", EMMC_0_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/phy@%08x", EMMC_0_PHY_BASE);
	plat_set_prop_disabled(blob, node_name);
	plat_set_prop_disabled(blob, "/fixed-regulator_1v8");
	plat_set_prop_disabled(blob, "/fixed-regulator_3v3");
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/ethernet@%08x", EMAC_1G_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/adrv906x_net@%08X", EMAC_CMN_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mdio@%08X", EMAC_PCS_0_BASE);
	plat_set_prop_disabled(blob, node_name);
	plat_set_prop_disabled(blob, "/ptpclk");
	ret = get_dual_tile(&is_dual_tile);
	if ((ret == 0) && (is_dual_tile == 1)) {
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/adrv906x_net@%08X", SEC_EMAC_CMN_BASE);
		plat_set_prop_disabled(blob, node_name);
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mdio@%08X", SEC_EMAC_PCS_0_BASE);
		plat_set_prop_disabled(blob, node_name);
	}

	/* Disable SPI0 and I2C1 */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/spi@%08X", SPI_0_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/twi@%08X", I2C_1_BASE);
	plat_set_prop_disabled(blob, node_name);

	/* Enable SystemC eMMC, SD, and ethernet */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mmc1@%x", 1);
	plat_set_prop_okay(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mmc0@%x", 0);
	plat_set_prop_okay(blob, node_name);
	plat_set_prop_okay(blob, "/sysc-fixed-regulator-0");
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/ethernet@%x", 2);
	plat_set_prop_okay(blob, node_name);

	/* Enable UART4 (PL011 UART3) for the virtual UART workaround */
	ret = get_secondary_linux_enabled(&is_secondary_linux_enabled);
	if ((ret == 0) && (is_secondary_linux_enabled == 1)) {
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/uart@%08x", PL011_3_BASE);
		node = fdt_path_offset(blob, node_name);
		if (node < 0) {
			plat_error_message("Missing %s node in device tree", node_name);
			return node;
		}
		ret = fdt_setprop_string(blob, node, "status", "okay");
		if (ret < 0) {
			plat_error_message("Unable to set status prop for node %s", node_name);
			return ret;
		}
	}

	return 0;
}

/* TODO: Consider removing if/when Protium/Palladium support is removed */
static int plat_protium_palladium_fixup(void *blob)
{
	int ret;
	char node_name[MAX_NODE_NAME_LENGTH];
	uint32_t is_dual_tile;

	/* Disable ethernet */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/ethernet@%08X", EMAC_1G_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/adrv906x_net@%08X", EMAC_CMN_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mdio@%08X", EMAC_PCS_0_BASE);
	plat_set_prop_disabled(blob, node_name);
	plat_set_prop_disabled(blob, "/ptpclk");
	ret = get_dual_tile(&is_dual_tile);
	if ((ret == 0) && (is_dual_tile == 1)) {
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/adrv906x_net@%08X", SEC_EMAC_CMN_BASE);
		plat_set_prop_disabled(blob, node_name);
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mdio@%08X", SEC_EMAC_PCS_0_BASE);
		plat_set_prop_disabled(blob, node_name);
	}

	/* Disable SPI0 and I2C1 */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/spi@%08X", SPI_0_BASE);
	plat_set_prop_disabled(blob, node_name);
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/twi@%08X", I2C_1_BASE);
	plat_set_prop_disabled(blob, node_name);

	/* Disable HS400 mode */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mmc@%08x", EMMC_0_BASE);
	plat_del_prop(blob, node_name, "mmc-hs400-1_8v");
	plat_del_prop(blob, node_name, "mmc-hs400-enhanced-strobe");

	return 0;
}

int adrv906x_kernel_fdt_fixup(void *blob)
{
	int clk_freq;
	int node;
	char node_name[MAX_NODE_NAME_LENGTH];
	uint32_t is_dual_tile;
	uint32_t is_secondary_linux_enabled;
	int ret;

	/* Set the dual-tile param, if dual-tile is enabled */
	ret = get_dual_tile(&is_dual_tile);
	if ((ret == 0) && (is_dual_tile == 1)) {
		node = fdt_path_offset(blob, "/chosen/boot");
		if (node < 0)
			return node;
		ret = fdt_setprop_u32(blob, node, "dual-tile", is_dual_tile);
		if (ret < 0)
			return ret;
	}

	/* Set the secondary-linux-enabled param, if enabled */
	ret = get_secondary_linux_enabled(&is_secondary_linux_enabled);
	if ((ret == 0) && (is_secondary_linux_enabled == 1)) {
		node = fdt_path_offset(blob, "/chosen/boot");
		if (node < 0)
			return node;
		ret = fdt_setprop_u32(blob, node, "secondary-linux-enabled", is_secondary_linux_enabled);
		if (ret < 0)
			return ret;
	}

	/* Set the sysclk frequency */
	clk_freq = get_sysclk_freq();
	if (clk_freq < 0)
		return clk_freq;
	node = fdt_path_offset(blob, "/sysclk");
	if (node < 0)
		return node;
	ret = fdt_setprop_u32(blob, node, "clock-frequency", (uint32_t)clk_freq);
	if (ret < 0)
		return ret;

	/* Set the hsdigclk frequency */
	clk_freq = get_hsdigclk_freq();
	if (clk_freq < 0)
		return clk_freq;
	node = fdt_path_offset(blob, "/hsdigclk");
	if (node < 0)
		return node;
	ret = fdt_setprop_u32(blob, node, "clock-frequency", (uint32_t)clk_freq);
	if (ret < 0)
		return ret;

	/* Update the SDHCI mmcclk freq and maximum base clock values based on HSDIG */
	clk_freq = get_emmcclk_freq();
	if (clk_freq < 0)
		return clk_freq;

	node = fdt_path_offset(blob, "/mmcclk");
	if (node < 0)
		return node;

	ret = fdt_setprop_u32(blob, node, "clock-frequency", (uint32_t)clk_freq);
	if (ret < 0)
		return ret;

	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/mmc@%08x", EMMC_0_BASE);
	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		plat_error_message("Missing %s node in device tree", node_name);
		return node;
	}

	ret = fdt_setprop_u32(blob, node, "max-frequency", (uint32_t)clk_freq);
	if (ret < 0) {
		plat_error_message("Unable to set status prop for node %s", node_name);
		return ret;
	}

	/* For secondary Linux enabled systems, enable the virtual UART */
	ret = get_secondary_linux_enabled(&is_secondary_linux_enabled);
	if ((ret == 0) && (is_secondary_linux_enabled == 1)) {
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/v_uart@%08x", VIRTUAL_PL011_0_0_BASE);
		node = fdt_path_offset(blob, node_name);
		if (node < 0) {
			plat_error_message("Missing %s node in device tree", node_name);
			return node;
		}
		ret = fdt_setprop_string(blob, node, "status", "okay");
		if (ret < 0) {
			plat_error_message("Unable to set status prop for node %s", node_name);
			return ret;
		}
	}

	ret = plat_memory_regions_fixup(blob);
	if (ret < 0) {
		plat_error_message("Unable to fix up memory regions");
		return -1;
	}

	ret = plat_uio_fixup(blob);
	if (ret < 0) {
		plat_error_message("Unable to fix up secondary uio nodes");
		return -1;
	}

	ret = plat_eth_fixup(blob);
	if (ret < 0) {
		plat_error_message("Unable to fix up ethernet mac addresses");
		return -1;
	}

	/* TODO: Consider removing if/when SystemC support is removed */
	if (is_sysc() == true) {
		ret = plat_sysc_fixup(blob);
		if (ret < 0) {
			plat_error_message("Unable to configure SystemC-specific devices");
			return ret;
		}
	}

	/* TODO: Consider removing if/when Protium/Palladium support is removed */
	if ((is_protium() == true) || (is_palladium() == true)) {
		ret = plat_protium_palladium_fixup(blob);
		if (ret < 0) {
			plat_error_message("Unable to configure Protium/Palladium-specific devices");
			return ret;
		}
	}

	return 0;
}

/*
 * Perform any device tree fixups needed before booting the kernel
 */
int ft_system_setup(void *blob, struct bd_info *bd)
{
	int ret;

	ret = common_kernel_fdt_fixup(blob);
	if (ret < 0)
		return ret;

	ret = adrv906x_kernel_fdt_fixup(blob);
	if (ret < 0)
		return ret;

	return ret;
}
