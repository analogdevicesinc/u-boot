#ifndef ADI_PWR_SC845_FAMILY_1000MHZ_CONFIG
#define ADI_PWR_SC845_FAMILY_1000MHZ_CONFIG

#if !defined(__ADSPSC846_FAMILY__)
# error Only suitable for ADSP-SC846 family parts.
#endif

#include "clock_config.h"

#if CONFIG_SHARC_CORE_CLOCK != SHARC_CORE_CLOCK_1000MHZ
# error CONFIG_SHARC_CORE_CLOCK not defined as expected
#endif

/** @addtogroup Init_Preload_SC84x Processor Initialization Code
 *  @{
 *
 */

/*!
* @file      adi_pwr_SC845_family_1000MHz_config.h
*
* @brief     Power Service configuration include file.
*
* @details   Definitions for the power service initialization.
*/

#include <sys/platform.h>

/**********************************************************************************************
 *                     CGU Configuration Number 0
 **********************************************************************************************/
/*
Configuration Number    : 0
SDRAM Mode              : DDR4
SYS_CLKIN0 (MHz)        : 25
Use CGU1 ?              : Yes

CDU Initialization Options
--------------------------
SHARC-FX & its Accelerators (CLKO0) : CCLK0_0          : 1000 MHz
ARM Cortex-A55              (CLKO1) : CCLK2_1          : 1200 MHz
DDR                         (CLKO3) : DCLK_0           :  500 MHz
CANFD                       (CLKO4) : OCLK_0           :  100 MHz
SPDIF                       (CLKO5) : SCLK1_0 (EXEN)   :  333 MHz
SPI                         (CLKO6) : SCLK0_0          :  125 MHz
GigE                        (CLKO7) : SCLK0_0          :  125 MHz
xSPI2                       (CLKO8) : SCLK1_1 (EXEN)   :  180 MHz
LP                          (CLKO9) : DCLK_1           :  300 MHz
xSPI                        (CLKO10): SCLK1_1 (EXEN)   :  180 MHz
TRACE                       (CLKO12): SCLK0_0          :  125 MHz
ePWM                        (CLKO13): SYSCLK_0         :  500 MHz
MSHC                        (CLKO14): SYSCLK_1         :  180 MHz

CGU0 Initialization Options
---------------------------
fVCO                   :   4000.00 MHz
fVCO/3                 :   1333.33 MHz
fVCO/5                 :    800.00 MHz
fPLL                   :   2000.00 MHz
CCLK                   :   1000.00 MHz
SYSCLK                 :    500.00 MHz
SCLK0                  :    125.00 MHz
SCLK1                  :    250.00 MHz
SCLK1_EXEN             :    333.33 MHz
DCLK                   :    500.00 MHz
OCLK                   :    100.00 MHz

MSEL                   :   80
CSEL                   :   1
CCLK to SYSCLK Ratio   :   2:1
SYSCLK to SCLK0 Ratio  :   4:1
SYSSEL                 :   2
S0SEL                  :   4
S1SEL                  :   2
DSEL                   :   2
OSEL                   :   10
Use S1SELEX?           :   Yes
S1SELEX                :   3

CGU1 Initialization Options
---------------------------
fVCO                   :   3600.00 MHz
fVCO/3                 :   1200.00 MHz
fVCO/5                 :    720.00 MHz
fPLL                   :   1800.00 MHz
CCLK                   :    450.00 MHz
SYSCLK                 :    180.00 MHz
SCLK0                  :     45.00 MHz
SCLK0_EXEN             :    112.50 MHz
SCLK1                  :     90.00 MHz
SCLK1_EXEN             :    180.00 MHz
DCLK                   :    300.00 MHz
OCLK                   :    112.50 MHz

MSEL                   :   72
CSEL                   :   2
SYSSEL                 :   5
S0SEL                  :   4
S1SEL                  :   2
DSEL                   :   3
OSEL                   :   8
Use S0SELEX?           :   Yes
S0SELEX                :   8
Use S1SELEX?           :   Yes
S1SELEX                :   5
*/

/**********************************************************************************************
 *                      CGU Configuration Number 0
 **********************************************************************************************/

#define CFG0_BIT_CGU0_CLKIN                                25000000 /*!< Macro for SYS_CLKIN */
#define CFG0_BIT_CGU1_CLKIN                                25000000 /*!< Macro for SYS_CLKIN */

/*****************************************CGU1_CLKINSELV**********************************************/
#define CFG0_BIT_CDU0_CFG0_SEL_VALUE                       (ENUM_CDU_CFG_IN0)        /*!< CLKO0 : SHARCFX 0 and its accelerators : CCLK0_0 CCLK0_1 CCLK2_0 */

#define CFG0_BIT_CDU0_CFG1_SEL_VALUE                       (ENUM_CDU_CFG_IN2)        /*!< CLKO1 : A55 : CCLK0_0	CCLK0_1	CCLK2_0 */

#define CFG0_BIT_CDU0_CFG3_SEL_VALUE                       (ENUM_CDU_CFG_IN0)        /*!< CLKO3 : DDR CLOCK : DCLK_0	DCLK_1	DCLK1_0 */

#define CFG0_BIT_CDU0_CFG4_SEL_VALUE                       (ENUM_CDU_CFG_IN0)        /*!< CLKO4 : CAN : OCLK_0 OCLK_1 */

#define CFG0_BIT_CDU0_CFG5_SEL_VALUE                       (ENUM_CDU_CFG_IN0)        /*!< CLKO5 : SPDIF : SCLK1_0 */

#define CFG0_BIT_CDU0_CFG6_SEL_VALUE                       (ENUM_CDU_CFG_IN0)        /*!< CLKO6 : SPI : SCLK0_0 OCLK_0 */

#define CFG0_BIT_CDU0_CFG7_SEL_VALUE                       (ENUM_CDU_CFG_IN0)        /*!< CLKO7 : GigE : SCLK0_0 SCLK0_1 */

#define CFG0_BIT_CDU0_CFG8_SEL_VALUE                       (ENUM_CDU_CFG_IN2)        /*!< CLKO8 : xSPI2 : SCLK0_0 OCLK_0 SCLK1_1 */

#define CFG0_BIT_CDU0_CFG9_SEL_VALUE                       (ENUM_CDU_CFG_IN2)        /*!< CLKO9 : LP : DCLK_0 CCLK2_0 DCLK_1 CCLK2_1 */

#define CFG0_BIT_CDU0_CFG10_SEL_VALUE                      (ENUM_CDU_CFG_IN2)        /*!< CLKO10 : xSPI : SCLK0_0 OCLK_0 SCLK1_1 */

#define CFG0_BIT_CDU0_CFG12_SEL_VALUE                      (ENUM_CDU_CFG_IN0)        /*!< CLKO12 : TRACE : SCLK0_0 */

#define CFG0_BIT_CDU0_CFG13_SEL_VALUE                      (ENUM_CDU_CFG_IN0)        /*!< CLKO13 : ePWM : SYSCLK_0 clkPWM */

#define CFG0_BIT_CDU0_CFG14_SEL_VALUE                      (ENUM_CDU_CFG_IN1)        /*!< CLKO14 : MSHC : SYSCLK_0 SYSCLK_1 */

/**********************************************************************************************
 *                     CGU Configuration Number 0 Register Values
 **********************************************************************************************/
/*****************************************CGU0_CTL**********************************************/
#define CFG0_BIT_CGU0_CTL_MSEL                             80       /*!< Macro for CGU0 MSEL field */
/*****************************************CGU0_DIV**********************************************/
#define CFG0_BIT_CGU0_DIV_CSEL                             1        /*!< Macro for CGU0 CSEL field */
#define CFG0_BIT_CGU0_DIV_SYSSEL                           2        /*!< Macro for CGU0 SYSSEL field */
#define CFG0_BIT_CGU0_DIV_S0SEL                            4        /*!< Macro for CGU0 S0SEL field */
#define CFG0_BIT_CGU0_DIV_S1SEL                            2        /*!< Macro for CGU0 S1SEL field */
#define CFG0_BIT_CGU0_DIV_DSEL                             2        /*!< Macro for CGU0 DSEL field */
#define CFG0_BIT_CGU0_DIV_OSEL                             10       /*!< Macro for CGU0 OSEL field */
/*****************************************CGU0_DIVEX**********************************************/
#define CFG0_BIT_CGU0_DIV_S1SELEX                          3        /*!< Macro for CGU0 S1SELEX field */

/*****************************************CGU1_CTL**********************************************/
#define CFG0_BIT_CGU1_CTL_MSEL                             72       /*!< Macro for CGU1 MSEL field */
/*****************************************CGU1_DIV**********************************************/
#define CFG0_BIT_CGU1_DIV_CSEL                             2        /*!< Macro for CGU1 CSEL field */
#define CFG0_BIT_CGU1_DIV_SYSSEL                           5        /*!< Macro for CGU1 SYSSEL field */
#define CFG0_BIT_CGU1_DIV_S0SEL                            4        /*!< Macro for CGU1 S0SEL field */
#define CFG0_BIT_CGU1_DIV_S1SEL                            2        /*!< Macro for CGU1 S1SEL field */
#define CFG0_BIT_CGU1_DIV_DSEL                             3        /*!< Macro for CGU1 DSEL field */
#define CFG0_BIT_CGU1_DIV_OSEL                             8        /*!< Macro for CGU1 OSEL field */
/*****************************************CGU1_DIVEX**********************************************/
#define CFG0_BIT_CGU1_DIV_S0SELEX                          8        /*!< Macro for CGU1 S1SELEX field */
#define CFG0_BIT_CGU1_DIV_S1SELEX                          5        /*!< Macro for CGU1 S1SELEX field */

/*@}*/

#endif /* #ifndef ADI_PWR_SC845_FAMILY_1000MHZ_CONFIG */
