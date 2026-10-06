// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written by Timesys Corporation
 *
 *
 * Analog Devices SC5xx remoteproc driver for loading code onto SHARC cores
 */

#include <dm.h>
#include <elf.h>
#include <malloc.h>
#include <regmap.h>
#include <remoteproc.h>
#include <syscon.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/io.h>

/* Register offsets */
#ifdef CONFIG_SC58X
#define ADI_RCU_REG_CTL		0x00
#define ADI_RCU_REG_STAT	0x04
#define ADI_RCU_REG_CRCTL	0x08
#define ADI_RCU_REG_CRSTAT	0x0c
#define ADI_RCU_REG_SIDIS	0x10
#define ADI_RCU_REG_SISTAT	0x14
#define ADI_RCU_REG_BCODE	0x1c
#define ADI_RCU_REG_SVECT0	0x20
#define ADI_RCU_REG_SVECT1	0x24
#define ADI_RCU_REG_SVECT2	0x28
#define ADI_RCU_REG_MSG		0x60
#define ADI_RCU_REG_MSG_SET	0x64
#define ADI_RCU_REG_MSG_CLR	0x68
#else
#define ADI_RCU_REG_CTL		0x00
#define ADI_RCU_REG_STAT	0x04
#define ADI_RCU_REG_CRCTL	0x08
#define ADI_RCU_REG_CRSTAT	0x0c
#define ADI_RCU_REG_SRRQSTAT	0x18
#define ADI_RCU_REG_SIDIS	0x1c
#define ADI_RCU_REG_SISTAT	0x20
#define ADI_RCU_REG_SVECT_LCK	0x24
#define ADI_RCU_REG_BCODE	0x28
#define ADI_RCU_REG_SVECT0	0x2c
#define ADI_RCU_REG_SVECT1	0x30
#define ADI_RCU_REG_SVECT2	0x34
#define ADI_RCU_REG_MSG		0x6c
#define ADI_RCU_REG_MSG_SET	0x70
#define ADI_RCU_REG_MSG_CLR	0x74
#endif /* CONFIG_SC58X */

/* Register bit definitions */
#define ADI_RCU_CTL_SYSRST		BIT(0)

/* Bit values for the RCU0_MSG register */
#define RCU0_MSG_C0IDLE			0x00000100		/* Core 0 Idle */
#define RCU0_MSG_C1IDLE			0x00000200		/* Core 1 Idle */
#define RCU0_MSG_C2IDLE			0x00000400		/* Core 2 Idle */
#define RCU0_MSG_CRR0			0x00001000		/* Core 0 reset request */
#define RCU0_MSG_CRR1			0x00002000		/* Core 1 reset request */
#define RCU0_MSG_CRR2			0x00004000		/* Core 2 reset request */
#define RCU0_MSG_C1ACTIVATE		0x00080000		/* Core 1 Activated */
#define RCU0_MSG_C2ACTIVATE		0x00100000		/* Core 2 Activated */

#define SHARCFX_IRAM_START		0x2F800000
#define SHARCFX_IRAM_END		0x2F80FFFF
#define SHARCFX_IRAM_ARM_OFFSET		0x07540000

#define SHT_ADI_ATTRIBUTES	(SHT_LOPROC + 2)
#define ADI_ATTR_SECTION_NAME	".adi.attributes"
#define ADI_ATTR_FORMAT_A	'A'
#define ADI_ATTR_VENDOR		"AnonADI"
#define VENDOR_ATTR_SIZE	8

#define ADI_ATTR_SUB_FILE	1
#define ADI_ATTR_SUB_SECTION	2

#define ADI_ATTR_TAG_PART	4
#define ADI_ATTR_TAG_WIDTH_BITS	19

/* SHARC1/SHARC2 L1 multiprocessor window offsets (DS Table 4) */
#define SHARC1_MP_OFFSET	0x28000000
#define SHARC2_MP_OFFSET	0x28800000

enum sc5xx_rproc_variant {
	SC5XX_RPROC_SHARC,	/* SHARC+ */
	SC5XX_RPROC_SHARCFX,	/* SHARC-FX */
};

enum sc5xx_firmware_variant {
	SC5XX_FW_NONE,
	SC5XX_FW_LDR,
	SC5XX_FW_ELF,
};

typedef struct __attribute__((__packed__)) adi_section {
	uint8_t format;
	uint32_t section_length;
	char vendor[VENDOR_ATTR_SIZE];
} adi_section;

typedef struct __attribute__((__packed__)) adi_section_property {
	uint8_t type;
	uint32_t length;
} adi_section_property;

typedef struct __attribute__((__packed__)) adi_tag {
	uint8_t id;
	uint8_t val;
} adi_tag;

typedef struct __attribute__((__packed__)) section_property {
	uint32_t addr;
	uint32_t bits;
} section_property;

typedef struct elf_section_list {
	uint32_t shnum;
	section_property *shproperty;
} elf_section_list;

struct sc5xx_rproc_data {
	/* Address to load to svect when rebooting core */
	u32 load_addr;

	/* RCU parameters */
	struct regmap *rcu;
	u32 svect_offset;
	u32 coreid;
	struct elf_section_list elf_sections;

	enum sc5xx_rproc_variant variant;
};

struct block_code_flag {
	u32 bcode:4,		/* 0-3 */
	    bflag_save:1,	/* 4 */
	    bflag_aux:1,	/* 5 */
	    breserved:1,	/* 6 */
	    bflag_forward:1,	/* 7 */
	    bflag_fill:1,	/* 8 */
	    bflag_quickboot:1,	/* 9 */
	    bflag_callback:1,	/* 10 */
	    bflag_init:1,	/* 11 */
	    bflag_ignore:1,	/* 12 */
	    bflag_indirect:1,	/* 13 */
	    bflag_first:1,	/* 14 */
	    bflag_final:1,	/* 15 */
	    bhdrchk:8,		/* 16-23 */
	    bhdrsign:8;		/* 0xAD, 0xAC or 0xAB */
};

struct ldr_hdr {
	struct block_code_flag bcode_flag;
	u32 target_addr;
	u32 byte_count;
	u32 argument;
};

#define WORD_SCALE_8 1
#define WORD_SCALE_16 2
#define WORD_SCALE_32 4
#define WORD_SCALE_48 6
#define WORD_SCALE_64 8

struct sharcp_space {
	uint32_t start;
	uint32_t end;
	uint32_t byte_base;
	uint8_t l1;
	uint8_t scale;
	uint8_t bits;
};

static const struct sharcp_space sharcp_spaces[] = {
	/* L1 block 0 */
	{ 0x00048000, 0x0004dfff, 0x00240000, 1, WORD_SCALE_64, 64 },
	{ 0x00090000, 0x00097fff, 0x00240000, 1, WORD_SCALE_48, 48 },
	{ 0x00090000, 0x0009bfff, 0x00240000, 1, WORD_SCALE_32, 32 },
	{ 0x00120000, 0x00137fff, 0x00240000, 1, WORD_SCALE_16, 16 },
	{ 0x00240000, 0x0026ffff, 0x00240000, 1, WORD_SCALE_8,  8  },
	/* L1 block 1 */
	{ 0x00058000, 0x0005dfff, 0x002c0000, 1, WORD_SCALE_64, 64 },
	{ 0x000b0000, 0x000b7fff, 0x002c0000, 1, WORD_SCALE_48, 48 },
	{ 0x000b0000, 0x000bbfff, 0x002c0000, 1, WORD_SCALE_32, 32 },
	{ 0x00160000, 0x00177fff, 0x002c0000, 1, WORD_SCALE_16, 16 },
	{ 0x002c0000, 0x002effff, 0x002c0000, 1, WORD_SCALE_8,  8  },
	/* L1 block 2 */
	{ 0x00060000, 0x00063fff, 0x00300000, 1, WORD_SCALE_64, 64 },
	{ 0x000c0000, 0x000c5554, 0x00300000, 1, WORD_SCALE_48, 48 },
	{ 0x000c0000, 0x000c7fff, 0x00300000, 1, WORD_SCALE_32, 32 },
	{ 0x00180000, 0x0018ffff, 0x00300000, 1, WORD_SCALE_16, 16 },
	{ 0x00300000, 0x0031ffff, 0x00300000, 1, WORD_SCALE_8,  8  },
	/* L1 block 3 */
	{ 0x00070000, 0x00073fff, 0x00380000, 1, WORD_SCALE_64, 64 },
	{ 0x000e0000, 0x000e5554, 0x00380000, 1, WORD_SCALE_48, 48 },
	{ 0x000e0000, 0x000e7fff, 0x00380000, 1, WORD_SCALE_32, 32 },
	{ 0x001c0000, 0x001cffff, 0x00380000, 1, WORD_SCALE_16, 16 },
	{ 0x00380000, 0x0039ffff, 0x00380000, 1, WORD_SCALE_8,  8  },
	/* L2, shared between the cores, no multiprocessor offset */
	{ 0x00580000, 0x005d5554, 0x20000000, 0, WORD_SCALE_48, 48 }, // ? verify
	{ 0x08000000, 0x0807ffff, 0x20000000, 0, WORD_SCALE_32, 32 },
	{ 0x00b00000, 0x00bfffff, 0x20000000, 0, WORD_SCALE_16, 16 },
	{ 0x20000000, 0x201fffff, 0x20000000, 0, WORD_SCALE_8,  8  },
};

uint32_t u32le(uint32_t u32)
{
	return ((u32 >> 24) & 0xff)
		|| (((u32 >> 16) & 0xff) << 8)
		|| (((u32 >> 8) & 0xff) << 16)
		|| ((u32 & 0xff) << 24);
}

static u8 sharc_section_bits(const struct elf_section_list *sections, u32 addr)
{
	unsigned int i;

	if (!sections->shproperty)
		return 0;

	for (i = 0; i < sections->shnum; i++) {
		if (sections->shproperty[i].addr == addr)
			return sections->shproperty[i].bits;
	}

	return 0;
}

int sharc_address_idx(uint32_t addr, uint8_t bits)
{
	int i;

	for (i = 0; i < sizeof(sharcp_spaces)/sizeof(sharcp_spaces[0]); i++) {
		const struct sharcp_space *map = &sharcp_spaces[i];
		if ((addr >= map->start) && (addr <= map->end) && (bits == map->bits)) {
			return i;
		}
	}

	return -1;
}

uint32_t sharcp_to_arm(uint32_t addr, uint8_t bits, int core)
{
	uint32_t arm_addr = 0x0;
	int idx = 0;
	const struct sharcp_space *sp = NULL;

	idx = sharc_address_idx(addr, bits);
	if (idx == -1)
		return 0x0;

	sp = &sharcp_spaces[idx];

	arm_addr = sp->byte_base + (addr - sp->start) * sp->scale;
	if (sp->l1) {
		arm_addr += (core == 2) ? SHARC2_MP_OFFSET : SHARC1_MP_OFFSET;
	}

	return arm_addr;
}

static int is_final(struct ldr_hdr *hdr)
{
	return hdr->bcode_flag.bflag_final;
}

static int is_empty(struct ldr_hdr *hdr)
{
	return hdr->bcode_flag.bflag_ignore || (hdr->byte_count == 0);
}

static int adi_valid_firmware(struct ldr_hdr *adi_ldr_hdr)
{
	if (!adi_ldr_hdr->byte_count &&
	    (adi_ldr_hdr->bcode_flag.bhdrsign == 0xAD ||
	     adi_ldr_hdr->bcode_flag.bhdrsign == 0xAC ||
	     adi_ldr_hdr->bcode_flag.bhdrsign == 0xAB))
		return 1;

	return 0;
}

static int sharc_ldr_load(struct udevice *dev, ulong addr, ulong size)
{
	struct sc5xx_rproc_data *priv = dev_get_priv(dev);
	size_t offset;
	u8 *buf = (u8 *)addr;
	struct ldr_hdr *block_hdr;
	struct ldr_hdr *next_hdr;


	do {
		block_hdr = (struct ldr_hdr *)buf;
		offset = sizeof(struct ldr_hdr) + (block_hdr->bcode_flag.bflag_fill ?
							0 : block_hdr->byte_count);
		next_hdr = (struct ldr_hdr *)(buf + offset);

		if (block_hdr->bcode_flag.bflag_first) {
			priv->load_addr = (unsigned long)block_hdr->target_addr;
		}

		if (!is_empty(block_hdr)) {

			if (block_hdr->bcode_flag.bflag_fill) {
				memset_io((void *)(phys_addr_t)block_hdr->target_addr,
					  block_hdr->argument,
					  block_hdr->byte_count);

			} else {
				memcpy_toio((void *)(phys_addr_t)block_hdr->target_addr,
					  buf + sizeof(struct ldr_hdr),
					  block_hdr->byte_count);
			}
		}

		if (is_final(block_hdr))
			break;

		buf += offset;
	} while (1);

	return 0;
}

static void sharc_elf_section_list_free(struct elf_section_list *sections)
{
	free(sections->shproperty);
	memset(sections, 0, sizeof(*sections));
}

static bool sharc_elf_range_ok(ulong image_size, u32 offset, u32 length)
{
	return offset <= image_size && length <= image_size - offset;
}

static int adi_attr_parse_section(struct elf_section_list *elfsh, const u8 *attr_section, u32 attr_size)
{
	int i = 0, j = 0;
	const adi_section *attribute_section = (const void *)attr_section;
	const uint8_t *attr_end = attr_section + 1 + attribute_section->section_length;
	const adi_section_property *property = (const void *)(attr_section + sizeof(*attribute_section));

	while ((const uint8_t *)property < attr_end) {
		uint32_t length = (property->length);
		uint32_t tag_length = length - sizeof(adi_section_property);
		uint32_t tag_num = tag_length/2;

		const uint8_t *tag_offset = (void *)((const uint8_t *)property + sizeof(adi_section_property));
		const adi_tag *tag = (const void *)(tag_offset);
		//fist tag is the section number, second is one of section memmory paramters
		if (tag_num < 2) {
			break;
		}

		if ((tag[0].id >= elfsh->shnum) && (tag[0].val != 0))
			break;

		for (i = 1; i < tag_num; i++) {
			//index 0 tag is section number and 0
			if (tag[i].id == ADI_ATTR_TAG_WIDTH_BITS) {
				uint32_t sec_id = tag[0].id;
				elfsh->shproperty[sec_id].bits = tag[i].val;
			}
		}

		property = (const void *)((const uint8_t *)property + property->length);
		j++;
	}

	//print all section that have address'es and bitness set
	for (i = 0; i < elfsh->shnum; i++) {
		uint32_t arm_addr = sharcp_to_arm(elfsh->shproperty[i].addr, elfsh->shproperty[i].bits, 0);
	}

	return 0;
}

static int sharc_elf_section_list_init(struct elf_section_list *elfsh,
					       ulong addr, ulong size)
{
	const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)addr;
	const Elf32_Shdr *shdr, *shstr, *attr = NULL;
	const char *section_names;
	unsigned int i;
	int ret;

	sharc_elf_section_list_free(elfsh);

	if (size < sizeof(*ehdr) || !ehdr->e_shoff || !ehdr->e_shnum ||
	    ehdr->e_shentsize != sizeof(*shdr) ||
	    ehdr->e_shstrndx >= ehdr->e_shnum)
		return -EINVAL;

	if (!sharc_elf_range_ok(size, ehdr->e_shoff, ehdr->e_shnum * sizeof(*shdr)))
		return -EINVAL;

	shdr = (const Elf32_Shdr *)(addr + ehdr->e_shoff);
	shstr = &shdr[ehdr->e_shstrndx];
	if (!sharc_elf_range_ok(size, shstr->sh_offset, shstr->sh_size))
		return -EINVAL;

	section_names = (const char *)(addr + shstr->sh_offset);
	elfsh->shnum = ehdr->e_shnum;
	elfsh->shproperty = calloc(elfsh->shnum, sizeof(*elfsh->shproperty));
	if (!elfsh->shproperty)
		return -ENOMEM;

	for (i = 0; i < elfsh->shnum; i++) {
		const Elf32_Shdr *section = &shdr[i];
		const char *name;

		if (section->sh_addr != 0x0)
			elfsh->shproperty[i].addr = section->sh_addr;

		if (section->sh_name >= shstr->sh_size)
			continue;

		name = section_names + section->sh_name;
		if (strnlen(name, shstr->sh_size - section->sh_name) >= shstr->sh_size - section->sh_name)
			continue;

		if (section->sh_type == SHT_ADI_ATTRIBUTES && strcmp(name, ADI_ATTR_SECTION_NAME) == 0)
			attr = section;
	}

	if (!attr) {
		ret = -ENOENT;
		goto err_free;
	}
	if (!sharc_elf_range_ok(size, attr->sh_offset, attr->sh_size)) {
		ret = -EINVAL;
		goto err_free;
	}
	ret = adi_attr_parse_section(elfsh,
				     (const u8 *)(addr + attr->sh_offset),
				     attr->sh_size);
	if (ret)
		goto err_free;
	return 0;

err_free:
	sharc_elf_section_list_free(elfsh);
	return ret;
}

/*
 * The .dxe stores every word of a word addressed SHARC space with the opposite
 * byte order to the one the ARM byte window presents; CCES's elfloader applies
 * that swap when it builds a .ldr, which is why the LDR path can memcpy its
 * blocks verbatim. rproc_elf32_load_image() copies the ELF payload as is, so
 * each word has to be rewritten in the right order afterwards - otherwise the
 * core resets to a valid SVECT, fetches byte reversed instructions and silently
 * does nothing. Byte space (scale 1) needs no swap.
 */
static void sharc_elf_swap_words(struct udevice *dev, ulong addr, ulong size)
{
	const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)addr;
	struct sc5xx_rproc_data *priv = dev_get_priv(dev);
	const Elf32_Phdr *phdr;
	unsigned int i, j;
	u32 off;

	phdr = (const Elf32_Phdr *)(addr + ehdr->e_phoff);

	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		const u8 *src;
		u8 *dst, scale;
		int idx;

		if (phdr->p_type != PT_LOAD || !phdr->p_filesz)
			continue;

		idx = sharc_address_idx(phdr->p_paddr,
					sharc_section_bits(&priv->elf_sections,
							   phdr->p_paddr));
		if (idx < 0)
			continue;

		scale = sharcp_spaces[idx].scale;
		if (scale < WORD_SCALE_16)
			continue;

		dst = (u8 *)(uintptr_t)sharcp_to_arm(phdr->p_paddr,
						     sharcp_spaces[idx].bits,
						     priv->coreid);
		if (!dst)
			continue;

		printk("swap da %08x arm_addr %p width %u\n", phdr->p_paddr, dst, scale);

		src = (const u8 *)(addr + phdr->p_offset);
		for (off = 0; off + scale <= phdr->p_filesz; off += scale)
			for (j = 0; j < scale; j++)
				writeb(src[off + j], dst + off + (scale - 1 - j));
	}
}

static int sharc_elf_load(struct udevice *dev, ulong addr, ulong size)
{
	u32 entry_point = rproc_elf_get_boot_addr(dev, addr);
	struct sc5xx_rproc_data *priv = dev_get_priv(dev);
	int ret;

	priv->load_addr = entry_point;

	if (priv->variant == SC5XX_RPROC_SHARC) {
		ret = sharc_elf_section_list_init(&priv->elf_sections, addr, size);
		if (ret) {
			dev_err(dev, "failed to parse ADI ELF attributes: %d\n",
				ret);
			return ret;
		}
	}

	ret = rproc_elf32_load_image(dev, addr, size);

	if (priv->variant == SC5XX_RPROC_SHARC) {
		if (!ret)
			sharc_elf_swap_words(dev, addr, size);
		sharc_elf_section_list_free(&priv->elf_sections);
	}


	return ret;
}

static int sharc_load(struct udevice *dev, ulong addr, ulong size)
{
	struct ldr_hdr *ldr = (struct ldr_hdr *)addr;
	int firmware_type = SC5XX_FW_NONE;

	if (adi_valid_firmware(ldr)) {
		firmware_type = SC5XX_FW_LDR;
	} else if (!rproc_elf32_sanity_check(addr, size)) {
		firmware_type = SC5XX_FW_ELF;
	} else {
		dev_err(dev, "Firmware at 0x%lx does not appear to be an ELF or LDR image\n", addr);
		return -EINVAL;
	}

	switch (firmware_type) {
	case SC5XX_FW_LDR:
		return sharc_ldr_load(dev, addr, size);
	case SC5XX_FW_ELF:
		return sharc_elf_load(dev, addr, size);
	}

	return -EINVAL;
}

static void sharc_reset(struct sc5xx_rproc_data *priv)
{
	u32 coreid = priv->coreid;
	u32 val;

	/* First put core in reset.
	 * Clear CRSTAT bit for given coreid.
	 */
	regmap_write(priv->rcu, ADI_RCU_REG_CRSTAT, 1 << coreid);

	/* Set SIDIS to disable the system interface */
	regmap_read(priv->rcu, ADI_RCU_REG_SIDIS, &val);
	regmap_write(priv->rcu, ADI_RCU_REG_SIDIS, val | (1 << (coreid - 1)));

	/*
	 * Wait for access to coreX have been disabled and all the pending
	 * transactions have completed
	 */
	udelay(50);

	/* Set CRCTL bit to put core in reset */
	regmap_read(priv->rcu, ADI_RCU_REG_CRCTL, &val);
	regmap_write(priv->rcu, ADI_RCU_REG_CRCTL, val | (1 << coreid));

	/* Poll until Core is in reset */
	while (!(regmap_read(priv->rcu, ADI_RCU_REG_CRSTAT, &val), val & (1 << coreid)))
		;

	/* Clear SIDIS to reenable the system interface */
	regmap_read(priv->rcu, ADI_RCU_REG_SIDIS, &val);
	regmap_write(priv->rcu, ADI_RCU_REG_SIDIS, val & ~(1 << (coreid - 1)));

	udelay(50);

	/* Take Core out of reset */
	regmap_read(priv->rcu, ADI_RCU_REG_CRCTL, &val);
	regmap_write(priv->rcu, ADI_RCU_REG_CRCTL, val & ~(1 << coreid));

	/* Wait for done */
	udelay(50);
}

static int sharc_start(struct udevice *dev)
{
	struct sc5xx_rproc_data *priv = dev_get_priv(dev);

	/* Write load address to appropriate SVECT for core */
	regmap_write(priv->rcu, priv->svect_offset, priv->load_addr);

	sharc_reset(priv);

	/* Clear the IDLE bit when start the SHARC core */
	regmap_write(priv->rcu, ADI_RCU_REG_MSG_CLR, RCU0_MSG_C0IDLE << priv->coreid);

	/* Notify CCES */
	regmap_write(priv->rcu, ADI_RCU_REG_MSG_SET, RCU0_MSG_C1ACTIVATE << (priv->coreid - 1));
	return 0;
}

void *sc5xx_sharc_pa_to_virt(struct udevice *dev, ulong da, ulong size)
{
	struct sc5xx_rproc_data *priv = dev_get_priv(dev);
	u32 coreid = priv->coreid;
	u32 arm_addr;
	u8 bits;

	//Instruction RAM
	//SHARC-FX -> ARM
	//0x2F800000–0x2F80FFFF -> 0x282C0000–0x282CFFFF 64KB
	if (priv->variant == SC5XX_RPROC_SHARCFX)
		if (da >= SHARCFX_IRAM_START && da <= SHARCFX_IRAM_END) {
			return (void *)(uintptr_t)(da - SHARCFX_IRAM_ARM_OFFSET);
		}

	bits = sharc_section_bits(&priv->elf_sections, da);
	if (!bits) {
		dev_err(dev, "no ADI section width for SHARC address 0x%lx\n", da);
		return da;
	}

	arm_addr = sharcp_to_arm(da, bits, coreid);
	if (arm_addr == 0x0) {
		dev_err(dev, "no SHARC memory map for address 0x%lx width %u\n",
			da, bits);
		return da;
	} else {
		return arm_addr;
	}

	return da;
}

static const struct dm_rproc_ops sc5xx_ops = {
	.load = sharc_load,
	.device_to_virt = sc5xx_sharc_pa_to_virt,
	.start = sharc_start,
};

static int sc5xx_probe(struct udevice *dev)
{
	struct sc5xx_rproc_data *priv = dev_get_priv(dev);
	u32 coreid;

	if (dev_read_u32(dev, "coreid", &coreid)) {
		dev_err(dev, "Missing property coreid\n");
		return -ENOENT;
	}

	priv->coreid = coreid;
	switch (coreid) {
	case 1:
		priv->svect_offset = ADI_RCU_REG_SVECT1;
		break;
	case 2:
		priv->svect_offset = ADI_RCU_REG_SVECT2;
		break;
	default:
		dev_err(dev, "Invalid value %d for coreid, must be 1 or 2\n", coreid);
		return -EINVAL;
	}

	priv->rcu = syscon_regmap_lookup_by_phandle(dev, "adi,rcu");
	if (IS_ERR(priv->rcu))
		return PTR_ERR(priv->rcu);

	priv->variant = dev_get_driver_data(dev);

	dev_err(dev, "sc5xx remoteproc core %d available\n", priv->coreid);

	return 0;
}

static const struct udevice_id sc5xx_ids[] = {
	{ .compatible = "adi,sc846-rproc", .data = SC5XX_RPROC_SHARCFX },
	{ .compatible = "adi,sc5xx-rproc", .data = SC5XX_RPROC_SHARC },
	{ }
};

U_BOOT_DRIVER(adi_sc5xx_rproc) = {
	.name = "adi_sc5xx_rproc",
	.of_match = sc5xx_ids,
	.id = UCLASS_REMOTEPROC,
	.ops = &sc5xx_ops,
	.probe = sc5xx_probe,
	.priv_auto = sizeof(struct sc5xx_rproc_data),
	.flags = 0,
};
