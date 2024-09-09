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
#define MAX_ENFORCEMENT_COUNTER_LENGTH     11

DECLARE_GLOBAL_DATA_PTR;

/*
 * Device tree as passed by TF-A
 * This must be placed in .data since .bss is not setup
 * when save_boot_params() is called.
 */
static unsigned long device_tree __section(".data") = 0;

/* Builds and set the SF probe command as an U-boot environment variable. Does not set
 * the environment variable if an error is detected with the SF device tree node*/
static void build_sf_probe_command(uint64_t qspi_0_base_addr)
{
	int node;
	int subnode;
	const void *blob;
	uint32_t bus_num;
	uint32_t cs_num;
	uint32_t spi_hz;
	uint32_t tx_width;
	uint32_t rx_width;
	uint32_t mode;
	char node_name[MAX_NODE_NAME_LENGTH];
	char sf_probe_params[SF_PARAMS_LEN];

	/* build sf probe params */
	blob = gd->fdt_blob;
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "/spi@%08llx", qspi_0_base_addr);
	node = fdt_path_offset(blob, node_name);
	if (node < 0) {
		log_err("Error: Failed to find sf node, not populating sf probe command. \n");
		return;
	}
	bus_num = fdtdec_get_int(blob, node, "bus-num", 0);

	subnode = fdt_first_subnode(blob, node);
	if (subnode < 0) {
		log_err("Error: Subnode with SPI flash config missing! Not Populating sf probe command. \n");
		return;
	}

	spi_hz = fdtdec_get_int(blob, subnode, "spi-max-frequency", 500000);
	cs_num = fdtdec_get_int(blob, subnode, "reg", 0);
	tx_width = fdtdec_get_int(blob, subnode, "spi-tx-bus-width", 0);
	rx_width = fdtdec_get_int(blob, subnode, "spi-rx-bus-width", 0);
	mode = 0;
	mode |= (tx_width == 4) ? SPI_TX_QUAD : SPI_TX_BYTE;
	mode |= (rx_width == 4) ? SPI_RX_QUAD : SPI_RX_SLOW;
	snprintf(sf_probe_params, SF_PARAMS_LEN, "sf probe %u:%u %u %x;", bus_num, cs_num, spi_hz, mode);
	env_set("adrv_sf_probe_cmd", sf_probe_params);
}

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
	char node_name[MAX_NODE_NAME_LENGTH];
	char enforcement_counter[MAX_ENFORCEMENT_COUNTER_LENGTH];
	uint32_t is_deployed;
	uint64_t kaslr_seed;
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

	/* Add anti-rollback node */
	snprintf(node_name, MAX_NODE_NAME_LENGTH, "anti-rollback");
	node = fdt_add_subnode(blob, node, node_name);
	if (node < 0)
		return node;

	/* Pass enforcement counter from U-Boot device tree to kernel device tree */
	ret = get_enforcement_counter();
	if (ret < 0)
		return ret;
	snprintf(enforcement_counter, MAX_ENFORCEMENT_COUNTER_LENGTH, "%d", (uint32_t)ret);
	ret = fdt_setprop_string(blob, node, "enforcement-counter", enforcement_counter);
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

	/* Set KASLR seed, if available */
	ret = get_kaslr_seed(&kaslr_seed);
	if (ret == 0) {
		node = fdt_path_offset(blob, "/chosen");
		if (node < 0)
			return node;
		ret = fdt_appendprop_u64(blob, node, "kaslr-seed", kaslr_seed);
		if (ret < 0)
			return ret;
	}

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
	uint32_t len;
	char *sf_probe_cmd;
	char sf_read_params[SF_PARAMS_LEN];
	char sf_load_cmd[SF_PARAMS_LEN];

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

	build_sf_probe_command(qspi_0_base_addr);

	if (is_boot_device_active(BOOT_DEV_QSPI_0)) {
		sf_probe_cmd = env_get("adrv_sf_probe_cmd");
		if (!sf_probe_cmd) {
			log_err("SF probe command not set. \n");
			return -1;
		}

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
		snprintf(sf_load_cmd, SF_PARAMS_LEN, sf_probe_cmd);
		len = snprintf(sf_read_params, SF_PARAMS_LEN, " sf read ${adrv_bootaddr} ${adrv_bootpartoff} ${adrv_bootpartsize}");
		strncat(sf_load_cmd, sf_read_params, len);
		env_set("adrv_dev_loadcmd", sf_load_cmd);
		put_mtd_device(mtd);
	} else if (is_boot_device_active(BOOT_DEV_EMMC_0) || is_boot_device_active(BOOT_DEV_SD_0)) {
		if (is_boot_device_active(BOOT_DEV_EMMC_0)) {
			/* Get a handle to the current boot device */
			ret = blk_get_device_by_str("mmc", "0", &desc);
			env_set("adrv_bootdevice", "mmc0");
			env_set("adrv_dev_loadcmd", "load mmc 0:${adrv_bootpart} ${adrv_bootaddr} kernel_fit.itb");
		} else {
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

		sf_probe_cmd = env_get("adrv_sf_probe_cmd");
		if (!sf_probe_cmd)
			log_warning("SF probe command not set, issues may arise if trying to write to SPI Flash.\n");
	} else {
		env_set("adrv_dev_loadcmd", "exit 1");
		env_set("adrv_bootdevice", "host");
		env_set("adrv_bootpartname", "N/A");
	}

	/* Get anti-rollback node */
	node = fdt_path_offset(gd->fdt_blob, "/boot/anti-rollback");
	if (node < 0) {
		log_err("/boot/anti-rollback node not found\n");
		return node;
	}

	/* Get enforcement-counter set from TF-A */
	ret = fdtdec_get_int(gd->fdt_blob, node, "enforcement-counter", 0);
	if (ret < 0) {
		log_err("Failed to get enforcement-counter\n");
		return ret;
	}
	env_set_ulong("enforcement_counter", ret);

	/* Build the command (and error checking) for loading the FIT from the boot device
	 * Get kernel FIT anti-rollback counter from custom field in the FIT configuration node
	 * If anti-rollback counter is less than enforcement counter, set reset cause
	 * and trigger warm reset
	 */
	env_set("adrv_loadcmd",
		"echo \"Loading kernel FIT from ${adrv_bootpartname} partition on ${adrv_bootdevice} to ${adrv_bootaddr}...\";" \
		"run adrv_dev_loadcmd;" \
		"if test $? -ne 0; then env set reset_cause \"OTHER_RESET_CAUSE\"; panic \"Panic: Failed to load FIT\"; fi;" \
		"fdt addr ${adrv_bootaddr} && " \
		"fdt get value dflt_config /configurations default && " \
		"fdt get value anti_rollback_version /configurations/${dflt_config} anti-rollback-version;" \
		"if test $? -ne 0; then env set reset_cause \"IMG_VERIFY_FAIL\"; panic \"Panic: Unable to get FIT anti-rollback version\"; fi;" \
		"if test $anti_rollback_version -lt $enforcement_counter; then env set reset_cause \"IMG_VERIFY_FAIL\"; panic \"Panic: FIT anti-rollback enforcement check failed\"; fi;");

	/* Setup 'adrv_bootcmd', which will be invoked during boot */
	env_set("adrv_bootcmd", "echo \"\";" \
		"run adrv_loadcmd;" \
		"echo \"\";" \
		"echo \"Booting FIT at ${adrv_bootaddr}...\";" \
		"bootm ${adrv_bootaddr};" \
		"if test $? -ne 0; then env set reset_cause \"OTHER_RESET_CAUSE\"; panic \"Panic: Failed to boot FIT\"; fi;");

	/* Build the command for the initial programming of the eMMC and SPI flash
	 * 1) Set addresses in RAM for storing the transferred TFTP files and the checksums for comparison
	 * 2) Initialize the count and failures
	 * 3) If the burnemmc variable is set for programming, initialize blkoffset to 0 and then until the count match the number of files set by the supervisor script, do the following:
	 *	a) Load the selected file and its checksum over tftp, using the format mmc-xxx and mmc-xxx-checksum
	 *	b) Write the mmc file to the eMMC with the mmc command
	 *	c) Read back from the same location in memory
	 *	d) Calculate the checksum from the readback, compare against the checksum loaded from tftp
	 *	e) If the checksums are different, increment the number of failures and repeat from step a) with the same file. Otherwise increment the count to the next file, and update the block offset/set failures back to 0.
	 *   f) If the number of failures for a certain file reaches 3, exit out of the programming script and report this failure to the host PC
	 * 4) If the burnnorflash variable is set, repeat the process from 3), swapping out the mmc read/write commands for spi flash read/write commands, and the mmc prefix for nor_flash, with the same threshold for failures.
	 * 5) If the command makes it through all the selected programming, report a success back to the host PC.
	 *
	 * NOTE: This command is only intended to be used as part of the initial programming tool in the tools directory and should not be called by anything else, as the required environment variables may not be set properly otherwise.
	 *		The tftploadaddr, crc1addr, crc2addr variables are addresses in DRAM that are platform specific and should be set by the respective platform. tftploadaddr should be set to a location with 256MB of available space to
	 *		write the tftp files, and crc1addr and crc2addr need 4 bytes each. An example of addresses could be tftploadaddr=0x43000000, crc1addr=0x42000000, crc2addr=0x42000004.
	 */
	env_set("initialprogrammingcommand",
		"setenv count 0 && setenv failures 0; " \
		"if test ${burnemmc} -ne 0; then " \
		"echo Programming emmc && " \
		"setenv blkoffset 0 && mmc dev 0 0; " \
		"if test $? -ne 0; then echo Failed to assign mmc device && echo MMC PROBE FAILURE && exit 1; fi;" \
		"while test ${count} -ne ${numemmcfiles}; " \
		"do setexpr filename fmt mmc-%03d ${count} && setexpr checksumfile fmt mmc-%03d-checksum ${count} && " \
		"tftp ${tftploadaddr} ${filename} && setenv emmcfilesize ${filesize} && setexpr emmcnumblocks ${emmcfilesize} / 0x200 && " \
		"mmc write ${tftploadaddr} ${blkoffset} ${emmcnumblocks} && tftp ${crc1addr} ${checksumfile} && mw ${tftploadaddr} 0 ${emmcfilesize} && " \
		"mmc read ${tftploadaddr} ${blkoffset} ${emmcnumblocks} && crc32 ${tftploadaddr} ${emmcfilesize} ${crc2addr} && cmp ${crc1addr} ${crc2addr} 0x1; " \
		"if test $? -ne 0; then setexpr failures ${failures} + 0x1; else setexpr count ${count} + 0x1 && setexpr blkoffset ${blkoffset} + ${emmcnumblocks} && setenv failures 0; fi; " \
		"if test ${failures} -gt 2; then echo PROGRAMMING FAILURE && exit 1; fi; "
		"done; fi; " \
		"if test ${burnnorflash} -ne 0; then " \
		"echo Programming SPI NOR Flash; " \
		"setenv count 0 && setenv offset 0 && run adrv_sf_probe_cmd; " \
		"if test $? -ne 0; then echo Failed to probe sf device && echo SF PROBE FAILURE && exit 1; fi;" \
		"while test ${count} -ne ${numnorflashfiles}; " \
		"do setexpr filename fmt nor_flash-%03d ${count} && setexpr checksumfile fmt nor_flash-%03d-checksum ${count} && " \
		"tftp ${tftploadaddr} ${filename} && setenv norflashfilesize ${filesize} && sf erase ${offset} +${norflashfilesize} && sf write ${tftploadaddr} ${offset} ${norflashfilesize} && " \
		"tftp ${crc1addr} ${checksumfile} && mw ${tftploadaddr} 0 ${norflashfilesize} && " \
		"sf read ${tftploadaddr} ${offset} ${norflashfilesize} && crc32 ${tftploadaddr} ${norflashfilesize} ${crc2addr} && cmp ${crc1addr} ${crc2addr} 0x1; " \
		"if test $? -ne 0; then setexpr failures ${failures} + 0x1; else setexpr count ${count} + 0x1; setexpr offset ${offset} + ${norflashfilesize}; setenv failures 0; fi; " \
		"if test ${failures} -gt 2; then echo PROGRAMMING FAILURE && exit 1; fi;" \
		"done; fi;" \
		"echo PROGRAMMING SUCCESS"
		);

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

int get_kaslr_seed(uint64_t *kaslr_seed)
{
	int len = 0;
	const fdt64_t *kaslr_node;
	int node;
	int ret;

	node = fdt_path_offset(gd->fdt_blob, "/boot");
	if (node < 0)
		return -1;

	kaslr_node = fdt_getprop((void *)gd->fdt_blob, node, "kaslr-seed", &len);
	if (!kaslr_node || len != sizeof(fdt64_t))
		return -1;
	*kaslr_seed = fdt64_to_cpu(*kaslr_node);

	/* Wipe the kaslr-seed entry after reading, like the kernel does */
	ret = fdt_setprop_u64((void *)gd->fdt_blob, node, "kaslr-seed", 0);

	return ret;
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

int get_enforcement_counter(void)
{
	int node;

	node = fdt_path_offset(gd->fdt_blob, "/boot/anti-rollback");
	if (node < 0)
		return -1;

	return fdtdec_get_uint(gd->fdt_blob, node, "enforcement-counter", -1);
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
