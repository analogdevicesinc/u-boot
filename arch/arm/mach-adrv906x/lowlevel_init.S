/*
 * Copyright (C) 2021 Analog Devices Inc.
 * Licensed under the GPL-2 or later.
 */

#include <asm-offsets.h>
#include <config.h>
#include <linux/linkage.h>
#include <asm/macro.h>
#include <asm/armv8/mmu.h>

#ifdef ADRV906X_UBOOT_STANDALONE

/***********************************************
 * ONLY NEEDED WHEN RUNNING U-BOOT STANDALONE,
 * i.e. without TF-A running first.
 ***********************************************/

/* This is a copy of the arch default lowlevel_init, with the
 * addition of the GICR PWRR bit set for ADRV906X's GIC600, which
 * is not in the default U-Boot git_init_secure_percpu implementation.
 */
ENTRY(lowlevel_init)
	mov	x29, lr			/* Save LR */

#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
	branch_if_slave x0, 1f
	ldr	x0, =GICD_BASE
	bl	gic_init_secure
1:
#if defined(CONFIG_GICV3)
	ldr	x0, =GICR_BASE
	mov     w10, #0x2
	str     w10, [x0, #0x24]        /* Set GIC600 PWRR enable */
	bl	gic_init_secure_percpu
#elif defined(CONFIG_GICV2)
	ldr	x0, =GICD_BASE
	ldr	x1, =GICC_BASE
	bl	gic_init_secure_percpu
#endif
#endif

#ifdef CONFIG_ARMV8_MULTIENTRY
	branch_if_master x0, x1, 2f

	/*
	 * Slave should wait for master clearing spin table.
	 * This sync prevent salves observing incorrect
	 * value of spin table and jumping to wrong place.
	 */
#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
#ifdef CONFIG_GICV2
	ldr	x0, =GICC_BASE
#endif
	bl	gic_wait_for_interrupt
#endif

	/*
	 * All slaves will enter EL2 and optionally EL1.
	 */
	adr	x4, lowlevel_in_el2
	ldr	x5, =ES_TO_AARCH64
	bl	armv8_switch_to_el2

lowlevel_in_el2:
#ifdef CONFIG_ARMV8_SWITCH_TO_EL1
	adr	x4, lowlevel_in_el1
	ldr	x5, =ES_TO_AARCH64
	bl	armv8_switch_to_el1

lowlevel_in_el1:
#endif

#endif /* CONFIG_ARMV8_MULTIENTRY */

2:
	mov	lr, x29			/* Restore LR */
	ret
ENDPROC(lowlevel_init)

#endif /* ADRV906X_UBOOT_STANDALONE */

