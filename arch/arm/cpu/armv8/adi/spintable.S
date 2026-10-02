// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2026 - Analog Devices, Inc.
 */

#include <config.h>
#include <linux/linkage.h>
#include <asm/macro.h>
#include <asm/system.h>
#include <asm/arch/mp.h>

.align 4
.global secondary_boot_addr
secondary_boot_addr:
	.quad __secondary_boot_func

.global secondary_boot_code_start
secondary_boot_code_start:
	.quad __secondary_boot_code_start

.global secondary_boot_code_size
secondary_boot_code_size:
	.quad __secondary_boot_code_end - __secondary_boot_code_start

.global secondary_boot_func
secondary_boot_func:
	.quad __secondary_boot_func

	/* Using 64 bit alignment since the spin table is accessed as data */
	.align 4
	/* Secondary Boot Code starts here */
__secondary_boot_code_start:
__spin_table:
	.space 8, 0x0
	.align 4
__secondary_boot_func:
	//EL3 securityy configuration
	mov x0, #0x531
	msr scr_el3, x0
	isb

	//counter frequency
	mov x0, #0xD650
	movk x0, #0x01DC, lsl 16
	msr cntfrq_el0, x0

	//GIC CPU interface
	mov x0, #0xF
	msr ICC_SRE_EL3, x0
	isb
	msr ICC_SRE_EL2, x0
	isb

	//GIC interrupt group enable
	mov x0, #0x3
	msr ICC_IGRPEN1_EL3, x0
	isb

	//GIC control priority
	msr ICC_CTLR_EL3, xzr
	msr ICC_CTLR_EL1, xzr
	isb
	mov x0, #0x80
	msr ICC_PMR_EL1, x0
	isb

	//GICD power on
	mov x2, #0x0
	movk x2, #0x3126, lsl 16
	str wzr, [x2, #0x24]
	ldr w3, [x2, #0x24]
	tbnz w3, #1, -4

	//GICD wake up
	ldr w3, [x2, #0x14]
	mov w4, #2
	bic w3, w3, w4
	str w3, [x2, #0x14]
	dsb st
	isb
	ldr w3, [x2, #0x14]
	tbnz w3, #2, -4

	//GICR Set All PPIs/SGIs to Group 1 Non-Secure
	mov x4, #0x0
	movk x4, #0x3127, lsl 16
	mov w3, #0xFFFFFFFF
	str w3, [x4, #0x80]
	str wzr, [x4, #0xD00]

	//jump to EL2
	adr x0, #0x14
	msr elr_el3, x0
	mov x0, #0x3C9
	msr spsr_el3, x0
	eret

	//spin table loop
	adr x0, __spin_table
0:
	wfe
	ldr x1, [x0]
	cbz x1, 0b
	br x1

_dead_loop:
	wfe
	b _dead_loop

__secondary_boot_code_end: