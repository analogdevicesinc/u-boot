/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2024 Analog Devices, Inc.
 */

#include <asm/global_data.h>
#include <asm/armv8/mmu.h>
#include <errno.h>
#include <env.h>
#include <mmc.h>
#include <fdtdec.h>
#include <init.h>
#include <log.h>
#include <stdio.h>
#include <dm/device.h>

#include <adrv906x.h>
#include <adrv906x_def.h>
#include <adrv906x_fdt.h>
#include <adrv_common.h>
#include <err.h>

#ifdef ADRV906X_UBOOT_STANDALONE
#define ADI_ADRV906X_TSGEN_BASE   0x20044000
#endif

#define BOOT_BASE_ADDRESS         0x48000000

DECLARE_GLOBAL_DATA_PTR;

static struct mm_region adrv906x_mem_map[] = {
	{
		/* Primary memory base addr and size is updated in dram_init() below.
		 * Don't change the position of this entry without updating dram_init()
		 */
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = 0x00000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	},{
		/* Secondary memory base addr and size is updated in dram_init() below.
		 * Don't change the position of this entry without updating dram_init()
		 */
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = 0x00000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	},
	{
		.virt = 0x20000000UL,
		.phys = 0x20000000UL,
		.size = 0x04000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	},{
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = adrv906x_mem_map;


int mach_cpu_init(void)
{
#ifdef ADRV906X_UBOOT_STANDALONE
	/* Enable TSGEN for standalone.
	 * This is done by TF-A in non-standalone.
	 */
	uint32_t reg;
	reg = readl((void *)ADI_ADRV906X_TSGEN_BASE);
	reg |= 0x1;
	writel(reg, (void *)ADI_ADRV906X_TSGEN_BASE);
#endif

	return 0;
}

int dram_init(void)
{
	uint32_t is_dual_tile;
	int ret;
	phys_addr_t secondary_ram_base = 0;
	phys_size_t secondary_ram_size = 0;

	/* Get the primary memory base addr and size from the device tree */
	ret = fdtdec_decode_ram_size(gd->fdt_blob, NULL, 0, (phys_addr_t *)&gd->ram_base, (phys_size_t *)&gd->ram_size, NULL);
	if (ret || (gd->ram_base == 0) || (gd->ram_size == 0)) {
		plat_error_message("Failed to extract memory info from 'memory' node device tree");
		return -ENXIO;
	}

	/* Get secondary memory base addr and size from the device tree.
	 */
	ret = get_dual_tile(&is_dual_tile);
	if ((ret == 0) && (is_dual_tile == 1)) {
		ret = fdtdec_decode_ram_size(gd->fdt_blob, "/memory-secondary", 0, &secondary_ram_base, &secondary_ram_size, NULL);
		if (ret) {
			plat_error_message("Failed to extract memory info from 'memory-secondary' node for secondary tile.");
			return -ENXIO;
		}
	}

	/* Update physical memory attrs in mem_map */

	/* Primary memory */
	adrv906x_mem_map[0].virt = gd->ram_base;
	adrv906x_mem_map[0].phys = gd->ram_base;
	adrv906x_mem_map[0].size = gd->ram_size;

	/* Secondary memory */
	adrv906x_mem_map[1].virt = secondary_ram_base;
	adrv906x_mem_map[1].phys = secondary_ram_base;
	adrv906x_mem_map[1].size = secondary_ram_size;

	return 0;
}

unsigned long board_get_usable_ram_top(unsigned long total_size)
{
	unsigned long top;
	unsigned long reserved_mem;

	/* Omit any potentially reserved regions from the top of DRAM
	 * so that U-Boot doesn't load the device tree or initrd there.
	 */
	if (is_sysc())
		reserved_mem = MAX_RESERVED_MEM_SIZE_SYSC;
	else
		reserved_mem = MAX_RESERVED_MEM_SIZE;

	top = gd->ram_base + gd->ram_size;
	if (reserved_mem < gd->ram_size)
		top -= reserved_mem;

	return top;
}

/* ADRV906X TODO: Remove this if/when SystemC support is removed */
int sysc_pl180_mmc_get_b_max(struct udevice *dev, void *dst, lbaint_t blkcnt)
{
	/* Workaround for SystemC PL180 eMMC/SD: Max block count must be fixed at 128 */
	return 128;
}

/* ADRV906x TODO: Remove this if/when SystemC support is removed */
static int arch_misc_init_sysc(void)
{
	int ret;

	/* Workaround for SystemC PL180 eMMC/SD: Max block count must be fixed at 128 */
	if (is_boot_device_active(BOOT_DEV_EMMC_0) ||
	    (is_boot_device_active(BOOT_DEV_SD_0))) {
		struct blk_desc *desc;
		struct mmc *mmc;
		struct dm_mmc_ops *ops;

		/* Get a handle to the current boot device */
		if (is_boot_device_active(BOOT_DEV_EMMC_0))
			ret = blk_get_device_by_str("mmc", "0", &desc);
		else
			ret = blk_get_device_by_str("mmc", "1", &desc);

		if (ret < 0) {
			plat_error_message("Failed to find boot device 0");
			return ret;
		}

		mmc = find_mmc_device(desc->devnum);
		ops = mmc_get_ops(mmc->dev);
		ops->get_b_max = sysc_pl180_mmc_get_b_max;
	}

	return 0;
}

static int arch_misc_init_adrv906x(void)
{
	int ret;
	char addr[16];
	char *loadcmd;
	uint32_t is_secondary_linux_enabled;

	/* ADRV906x TODO: Remove this if/when SystemC support is removed */
	if (true == is_sysc()) {
		ret = arch_misc_init_sysc();
		if (ret < 0)
			return ret;
	}

	/* ADRV906x: TODO: Remove this and its usage in the secondary loadcmd
	 * below if/when SystemC support is removed */
	if (is_sysc() == true)
		env_set("sec_uart4_status", "okay");
	else
		env_set("sec_uart4_status", "disabled");
	snprintf(addr, 11, "%08X", SEC_PL011_3_BASE);
	env_set("sec_uart4_addr", addr);

	/* Secondary tile loadcmd */
	ret = get_secondary_linux_enabled(&is_secondary_linux_enabled);
	if ((ret == 0) && (is_secondary_linux_enabled == 1)) {
		/* If secondary Linux is enabled:
		 * - Build the command for loading the kernel, initramfs, and
		 *   device tree to the correct locations in secondary DRAM
		 * - Fixup secondary kernel DTB
		 */

		/* Calculate clock freqs and addresses */
		env_set_ulong("sec_sysclk_freq", (uint32_t)get_sysclk_freq());
		snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[1].virt);
		env_set("sec_memory_base", addr);
		snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[1].size);
		env_set("sec_memory_size", addr);
		snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[1].virt + 0x00001000);
		env_set("sec_fdt_dst_addr", addr);
		snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[1].virt + 0x00200000);
		env_set("sec_kernel_dst_addr", addr);
		snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[1].virt + 0x02200000);
		env_set("sec_initrd_dst_addr", addr);

		/* Save off the existing loadcmd, which will be replaced by a
		 * command that includes the secondary load steps.
		 */
		loadcmd = env_get("adrv_loadcmd");
		env_set("adrv906x_fit_loadcmd", loadcmd);

		/* Pass the platform type to the secondary */
		env_set("sec_platform", get_platform());

		/* Build the command to setup the secondary. Summary:
		 * 1) Run fdt checksign and iminfo to verify signature
		 * 2) Extract kernel, initramfs, and fdt locations from FIT
		 * 3) Copy device tree from FIT to secondary DRAM
		 * 4) Copy initramfs from FIT to secondary DRAM
		 *    Note: This breaks the copy up into 1MB chunks to workaround
		 *    SystemC C2C slowness (which causes a WDT timeout).
		 *    TODO: Consider removing if/when SystemC support is dropped
		 * 5) Copy kernel from FIT to secondary DRAM and decompress
		 * 6) Do device tree fixup on secondary device tree:
		 *    -Set lifecycle info
		 *    -Set initramfs locations in chosen node
		 *    -Set memory info
		 *    -Set reserved (user space) memory info
		 *     Note: tricky set of operations to obtain the size value
		 *     from device tree (replicating 'fdt32_to_cpu' endianness
		 *     conversion for this platform)
		 *    -Set sysclk freq
		 *    -Set UART4 enable (TODO: Remove SystemC workaround)
		 *    -Set platform type
		 * 7) Set secondary bootargs
		 *    -Device tree addr
		 *    -Kernel addr
		 *    -Magic number (0xAD12B007)
		 * 8) Flush dcache to flush writes from #8 to DRAM
		 *    -This is excessive, but U-Boot only exposes a command
		 *     to flush the entire dcache.
		 *     TODO: Consider optimizing this
		 * 9) If anything fails, remove the dual-tile and
		 *    secondary-linux-enabled flags from primary device tree
		 *    to disable further secondary boot
		 */
		env_set("adrv906x_secondary_loadcmd",
			"setenv sec_loadcmd_fail 0 && " \
			"fdt addr ${adrv_bootaddr} && " \
			"fdt checksign && iminfo ${adrv_bootaddr} && " \
			"fdt get value dflt_config /configurations default && " \
			"fdt get value kernel_img /configurations/${dflt_config} kernel && " \
			"fdt get value sec_fdt_img /configurations/${dflt_config} fdt-secondary && " \
			"fdt get value initrd_img /configurations/${dflt_config} ramdisk && " \
			"fdt get addr sec_kernel_src_addr /images/${kernel_img} data && " \
			"fdt get size sec_kernel_size /images/${kernel_img} data && " \
			"fdt get addr sec_fdt_src_addr /images/${sec_fdt_img} data && " \
			"fdt get size sec_fdt_size /images/${sec_fdt_img} data && " \
			"fdt get addr sec_initrd_src_addr /images/${initrd_img} data && " \
			"fdt get size sec_initrd_size /images/${initrd_img} data && " \
			"echo \"Loading secondary device tree to ${sec_fdt_dst_addr}...\" && " \
			"cp.b ${sec_fdt_src_addr} ${sec_fdt_dst_addr} ${sec_fdt_size} && " \
			"echo \"Loading secondary initramfs to ${sec_initrd_dst_addr}...\" && " \
			"setenv numbytes ${sec_initrd_size} && " \
			"setenv src ${sec_initrd_src_addr} && " \
			"setenv dst ${sec_initrd_dst_addr}; " \
			"if test $? -ne 0; then setenv sec_loadcmd_fail 1; fi; " \
			"test ${sec_loadcmd_fail} -eq 0 && while test ${numbytes} -ge 0x100000; " \
			"do cp.b ${src} ${dst} 0x100000 && setexpr numbytes ${numbytes} - 0x100000 && " \
			"setexpr src ${src} + 0x100000 && setexpr dst ${dst} + 0x100000 && " \
			"setenv numbytes 0x${numbytes}; " \
			"if test $? -ne 0; then setenv sec_loadcmd_fail 1; setenv numbytes 0; fi; done; " \
			"test ${sec_loadcmd_fail} -eq 0 && cp.b ${src} ${dst} ${numbytes} && " \
			"echo \"Loading secondary kernel to ${sec_kernel_dst_addr}...\" && " \
			"unzip ${sec_kernel_src_addr} ${sec_kernel_dst_addr} && " \
			"fdt addr ${fdtcontroladdr} && " \
			"fdt get value lifecycle_desc /boot/lifecycle-state description && " \
			"fdt get value lifecycle_deployed /boot/lifecycle-state deployed && " \
			"fdt addr ${sec_fdt_dst_addr} && " \
			"fdt resize && " \
			"fdt set /chosen/boot/lifecycle-state description \"${lifecycle_desc}\" && " \
			"fdt set /chosen/boot/lifecycle-state deployed <${lifecycle_deployed}> && " \
			"setexpr sec_initrd_dst_end_addr ${sec_initrd_dst_addr} + ${sec_initrd_size} && " \
			"fdt chosen ${sec_initrd_dst_addr} ${sec_initrd_dst_end_addr} && " \
			"fdt memory ${sec_memory_base} ${sec_memory_size} && " \
			"fdt get value regval /reserved-memory/ddr-reserved@0 reg && " \
			"setexpr a1 ${regval} / 1000000 && setexpr a1 ${a1} % 100 && " \
			"setexpr a2 ${regval} / 10000 && setexpr a2 ${a2} % 100 && setexpr a2 ${a2} * 100 && " \
			"setexpr a3 ${regval} / 100 && setexpr a3 ${a3} % 100 && setexpr a3 ${a3} * 10000 && " \
			"setexpr a4 ${regval} % 100 && setexpr a4 ${a4} * 1000000 && " \
			"setexpr res_size ${a1} + ${a2} && " \
			"setexpr res_size ${res_size} + ${a3} && " \
			"setexpr res_size ${res_size} + ${a4} && " \
			"setexpr res_addr ${sec_memory_base} && " \
			"fdt set /reserved-memory/ddr-reserved@0 reg <0x${res_addr} 0x${res_size}> && " \
			"fdt set /sysclk clock-frequency <${sec_sysclk_freq}> && " \
			"fdt set /uart@${sec_uart4_addr} status ${sec_uart4_status} && " \
			"fdt set /chosen/boot plat ${sec_platform} && " \
			"setexpr addr ${sec_memory_base} + 0x10 && " \
			"mw.q ${addr} ${sec_fdt_dst_addr} && " \
			"setexpr addr ${sec_memory_base} + 0x8 && " \
			"mw.q ${addr} ${sec_kernel_dst_addr} && " \
			"mw.q ${sec_memory_base} 0xAD12B007 && " \
			"dcache flush; " \
			"if test $? -ne 0; then setenv sec_loadcmd_fail 1; fi; " \
			"if test ${sec_loadcmd_fail} -eq 1; then echo Error: Secondary loading failed; " \
			"fdt addr ${fdtcontroladdr}; fdt resize; fdt rm /boot dual-tile; fdt rm /boot secondary-linux-enabled; fi;");

		/* Re-Build the loadcommand */
		env_set("adrv_loadcmd",
			"run adrv906x_fit_loadcmd;" \
			"run adrv906x_secondary_loadcmd;");
	}

	/* Set RAM load addresses for initial programming command */
	snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[0].virt + 0x01000000);
	env_set("tftploadaddr", addr);
	snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[0].virt);
	env_set("crc1addr", addr);
	snprintf(addr, 11, "0x%08llX", adrv906x_mem_map[0].virt + 0x4);
	env_set("crc2addr", addr);

	return 0;
}

int arch_misc_init(void)
{
	int ret;

	ret = arch_misc_init_common(BOOT_BASE_ADDRESS, QSPI_0_BASE);
	if (ret < 0)
		return ret;

	ret = arch_misc_init_adrv906x();
	if (ret < 0)
		return ret;

	return ret;
}
