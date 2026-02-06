/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#include <asm/system.h>
#include <bootstage.h>
#include <env.h>
#include <fdtdec.h>
#include <mmc.h>
#include <mtd.h>
#include <part.h>
#include <spi.h>
#include <stdio.h>

#include <adrv_common.h>
#include <err.h>

#define KERNEL_PART_STR_LEN  9
#define SF_PARAMS_LEN        128
#define BOOT_ADDR_LEN        20

DECLARE_GLOBAL_DATA_PTR;

/*
 * Device tree as passed by TF-A
 * This must be placed in .data since .bss is not setup
 * when save_boot_params() is called.
 */
static unsigned long device_tree __section(".data") = 0;

void reset_cpu(void)
{
	plat_error_handler();

	while (1)
		;
}

void show_boot_progress(int status)
{
	if (status == -BOOTSTAGE_ID_NEED_RESET) {
		printf("Resetting\n");
		plat_error_handler();
	}
}

/*
 * Save the FDT address provided by TF-A in r0 at boot time
 */
void save_boot_params(unsigned long r0, unsigned long r1, unsigned long r2,
		      unsigned long r3)
{
	device_tree = r0;
	save_boot_params_ret();
}

/*
 * Gets the address of the device tree passed by TF-A, if it is valid
 */
void *board_fdt_blob_setup(void)
{
	if (device_tree != 0)
		if (fdt_magic(device_tree) == FDT_MAGIC)
			return (void *)device_tree;

	return NULL;
}

int common_kernel_fdt_fixup(void *blob)
{
	const char *boot_device = NULL;
	const char *boot_slot = NULL;
	const char *platform;
	const char *lifecycle_description = NULL;
	uint32_t is_deployed;
	int node;
	int ret;

	/* Add the boot slot and boot device to the kernel device tree */
	node = fdt_path_offset(blob, "/chosen/boot");
	if (node < 0)
		return node;

	/* Set the boot device */
	ret = get_boot_device(&boot_device);
	if (ret < 0)
		return ret;
	ret = fdt_setprop_string(blob, node, "device", boot_device);
	if (ret < 0)
		return ret;

	/* Set the boot slot */
	ret = get_boot_slot(&boot_slot);
	if (ret < 0)
		return ret;
	ret = fdt_setprop_string(blob, node, "slot", boot_slot);
	if (ret < 0)
		return ret;

	/* Set the TE boot slot */
	ret = get_te_boot_slot(&boot_slot);
	if (ret < 0)
		return ret;
	ret = fdt_setprop_string(blob, node, "te-slot", boot_slot);
	if (ret < 0)
		return ret;

	/* Set the platform */
	platform = get_platform();
	ret = fdt_setprop_string(blob, node, "plat", platform);
	if (ret < 0)
		return ret;

	/* Set the lifecycle state */
	ret = get_lifecycle_state(&lifecycle_description, &is_deployed);
	if (ret < 0)
		return ret;
	node = fdt_path_offset(blob, "/chosen/boot/lifecycle-state");
	if (node < 0)
		return node;
	ret = fdt_setprop_string(blob, node, "description", lifecycle_description);
	if (ret < 0)
		return ret;
	ret = fdt_setprop_u32(blob, node, "deployed", is_deployed);
	if (ret < 0)
		return ret;

	return 0;
}

int arch_misc_init_common(uint64_t boot_addr, uint64_t qspi_0_base_addr)
{
	const char *boot_slot = NULL;
	char kern_part_name[KERNEL_PART_STR_LEN] = "kernel_X";
	char boot_addr_str[BOOT_ADDR_LEN];
	struct blk_desc *desc;
	struct disk_partition info;
	const char *boot_device = NULL;
	int part;
	int ret;
	struct mtd_info *mtd;
	int node;
	int subnode;
	const void *blob;
	uint32_t bus_num;
	uint32_t cs_num;
	uint32_t spi_hz;
	uint32_t tx_width;
	uint32_t rx_width;
	uint32_t mode;
	uint32_t len;
	char node_name[MAX_NODE_NAME_LENGTH];
	char sf_probe_params[SF_PARAMS_LEN];
	char sf_read_params[SF_PARAMS_LEN];

	/* Get the current boot device, verify it, then announce it */
	ret = get_boot_device(&boot_device);
	if (ret < 0) {
		log_err("Failed to read boot device\n");
		return ret;
	}
	if (!is_boot_device_active(BOOT_DEV_SD_0) && !is_boot_device_active(BOOT_DEV_EMMC_0) &&
	    !is_boot_device_active(BOOT_DEV_QSPI_0) && !is_boot_device_active(BOOT_DEV_HOST)) {
		log_err("Invalid boot device specified\n");
		return -1;
	}
	log_info("Active boot device: %s\n", boot_device);

	/* Get the current boot slot, verify it, then announce it */
	ret = get_boot_slot(&boot_slot);
	if (ret < 0) {
		log_err("Failed to read boot slot\n");
		return ret;
	}
	if (boot_slot[0] != 'a' && boot_slot[0] != 'b') {
		log_err("Invalid boot slot %c\n", boot_slot[0]);
		return -1;
	}
	log_info("Active boot slot: %c\n", boot_slot[0]);
	snprintf(kern_part_name, KERNEL_PART_STR_LEN, "kernel_%c", boot_slot[0]);

	snprintf(boot_addr_str, BOOT_ADDR_LEN, "0x%llx", boot_addr);
	env_set("adrv_bootaddr", boot_addr_str);

	if (is_boot_device_active(BOOT_DEV_QSPI_0)) {
		/* build sf probe params */
		blob = gd->fdt_blob;
		snprintf(node_name, MAX_NODE_NAME_LENGTH, "/spi@%08x", qspi_0_base_addr);
		node = fdt_path_offset(blob, node_name);
		bus_num = fdtdec_get_int(blob, node, "bus-num", 0);

		subnode = fdt_first_subnode(blob, node);
		if (subnode < 0) {
			log_err("Error: subnode with SPI flash config missing!\n");
			return -1;
		}

		spi_hz = fdtdec_get_int(blob, subnode, "spi-max-frequency", 500000);
		cs_num = fdtdec_get_int(blob, subnode, "reg", 0);
		tx_width = fdtdec_get_int(blob, subnode, "spi-tx-bus-width", 0);
		rx_width = fdtdec_get_int(blob, subnode, "spi-rx-bus-width", 0);
		mode = 0;
		mode |= (tx_width == 4) ? SPI_TX_QUAD : SPI_TX_BYTE;
		mode |= (rx_width == 4) ? SPI_RX_QUAD : SPI_RX_SLOW;
		snprintf(sf_probe_params, SF_PARAMS_LEN, "sf probe %u:%u %u %x;", bus_num, cs_num, spi_hz, mode);

		mtd_probe_devices();
		mtd = get_mtd_device_nm(kern_part_name);
		if (IS_ERR_OR_NULL(mtd)) {
			log_err("Failed to find boot device 0\n");
			return -1;
		}
		env_set("adrv_bootdevice", "spi flash");
		env_set("adrv_bootpartname", mtd->name);
		env_set_hex("adrv_bootpartoff", (ulong)(mtd->offset));
		env_set_hex("adrv_bootpartsize", (ulong)(mtd->size));
		len = snprintf(sf_read_params, SF_PARAMS_LEN, " sf read ${adrv_bootaddr} ${adrv_bootpartoff} ${adrv_bootpartsize}");
		strncat(sf_probe_params, sf_read_params, len);
		env_set("adrv_dev_loadcmd", sf_probe_params);
		put_mtd_device(mtd);
	} else {
		if (is_boot_device_active(BOOT_DEV_EMMC_0)) {
			/* Get a handle to the current boot device */
			ret = blk_get_device_by_str("mmc", "0", &desc);
			env_set("adrv_bootdevice", "mmc0");
			env_set("adrv_dev_loadcmd", "load mmc 0:${adrv_bootpart} ${adrv_bootaddr} kernel_fit.itb");
		} else if (is_boot_device_active(BOOT_DEV_SD_0)) {
			ret = blk_get_device_by_str("mmc", "1", &desc);
			env_set("adrv_bootdevice", "mmc1");
			env_set("adrv_dev_loadcmd", "load mmc 1:${adrv_bootpart} ${adrv_bootaddr} kernel_fit.itb");
		}

		if (ret < 0) {
			log_err("Failed to find boot device 0\n");
			return ret;
		}
		/* Get a handle to the active boot slot's kernel partition */
		part = part_get_info_by_name(desc, kern_part_name, &info);
		if (part < 0) {
			log_err("Failed to find kernel_%c partition \n", *boot_slot);
			return part;
		}
		env_set_ulong("adrv_bootpart", part);
		env_set("adrv_bootpartname", kern_part_name);
	}

	/* Build the command (and error checking) for loading the FIT from the boot device */
	env_set("adrv_loadcmd",
		"echo \"Loading kernel FIT from ${adrv_bootpartname} partition on ${adrv_bootdevice} to ${adrv_bootaddr}...\";" \
		"run adrv_dev_loadcmd;" \
		"if test $? -ne 0; then env set reset_cause \"OTHER_RESET_CAUSE\"; panic \"Panic: Failed to load FIT\"; fi;");

	/* Setup 'adrv_bootcmd', which will be invoked during boot */
	env_set("adrv_bootcmd", "echo \"\";" \
		"run adrv_loadcmd;" \
		"echo \"\";" \
		"echo \"Booting FIT at ${adrv_bootaddr}...\";" \
		"bootm ${adrv_bootaddr};" \
		"if test $? -ne 0; then env set reset_cause \"OTHER_RESET_CAUSE\"; panic \"Panic: Failed to boot FIT\"; fi;");

	return 0;
}

int get_boot_slot(const char **boot_slot)
{
	int node;
	int len = 0;

	node = fdt_path_offset(gd->fdt_blob, "/boot");
	if (node < 0)
		return -1;

	*boot_slot = fdt_getprop(gd->fdt_blob, node, "slot", &len);

	return 0;
}

int get_boot_device(const char **boot_device)
{
	int node;
	int len = 0;

	node = fdt_path_offset(gd->fdt_blob, "/boot");
	if (node < 0)
		return -1;

	*boot_device = fdt_getprop(gd->fdt_blob, node, "device", &len);

	return 0;
}

int get_te_boot_slot(const char **boot_slot)
{
	int node;
	int len = 0;

	node = fdt_path_offset(gd->fdt_blob, "/boot");
	if (node < 0)
		return -1;

	*boot_slot = fdt_getprop(gd->fdt_blob, node, "te-slot", &len);

	return 0;
}

int get_lifecycle_state(const char **description, uint32_t *deployed)
{
	int node;
	int len = 0;

	node = fdt_path_offset(gd->fdt_blob, "/boot/lifecycle-state");
	if (node < 0)
		return -1;

	*description = fdt_getprop(gd->fdt_blob, node, "description", &len);
	*deployed = fdtdec_get_uint(gd->fdt_blob, node, "deployed", 1);

	return 0;
}

char *get_platform(void)
{
	int len;
	int node;
	char *plat;

	node = fdt_path_offset(gd->fdt_blob, "/boot");
	if (node < 0)
		return "";

	plat = (char *)fdt_getprop(gd->fdt_blob, node, "plat", &len);

	return plat;
}
