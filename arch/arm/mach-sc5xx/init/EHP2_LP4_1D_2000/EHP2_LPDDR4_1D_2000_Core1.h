/*****************************************************************************
 * EHP2_LPDDR4_1D_2000_Core1.h
 *****************************************************************************/

#ifndef __EHP2_LPDDR4_1D_2000_CORE1_H__
#define __EHP2_LPDDR4_1D_2000_CORE1_H__


/* Add your custom header content here */

/*  Manually added */
#define MISCREG_MISC_REG_LPDDR4_RSTCTL       0x310A9400
#define REG_LPDDRMISC_DBG_CTL0               0x3115C008            /*  LPDDRMISC Debug Control 0 */

#define MOD_UMCTL2_REGS_BASE           REG_LPDDR4_CTLR
#define MOD_DPHY_APBONLY0_BASE 		   REG_LPDDR4_PHY_APB_MICROCONTMUXSEL

#define MEM_START 	0x80000000
#define BYTE_COUNT	0x400000         /* Testing 8Mbit */

#endif /* __EHP2_LPDDR4_1D_2000_CORE1_H__ */
