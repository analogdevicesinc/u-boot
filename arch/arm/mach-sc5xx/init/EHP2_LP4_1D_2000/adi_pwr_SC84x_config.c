/*
 ** Copyright (C) 2021-2025 Analog Devices Inc., All Rights Reserved.
 **
 ** This file was originally generated based upon the options selected in
 ** the Basic Configuration of CGU Initialization configuration dialog.
 ** Subsequently it has been manually edited.
 */
/** @addtogroup Init_Preload_SC84x Processor Initialization Code
 *  @{
 *
 */

/*!
* @file      adi_pwr_SC84x_config.c
*
* @brief     power Service configuration file
*
* @details
*            power Service configuration file
*/

#include <sys/platform.h>
#include <stdint.h>
#include <stdlib.h>
#include <services/pwr/adi_pwr.h>
#include "adi_pwr_SC84x_config.h"

/**
 * @brief    Initializes clocks, including CGU and CDU modules.
 *
 * @return   Status
 *           - 0: Successful in all the initializations.
 *           - 1: Error.

 */
uint32_t adi_pwr_cfg0_init()
{
    uint32_t status = 0u; /*Return zero if there are no errors*/

    /* Structure pointer for CGU0 and CGU1 parameters*/
    ADI_PWR_CGU_PARAM_LIST pADI_CGU_Param_List;

    /* Structure pointer for CDU parameters*/
    ADI_PWR_CDU_PARAM_LIST pADI_CDU_Param_List;

    /* CDU Configuration*/
    pADI_CDU_Param_List.cdu_settings[0].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG0_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[0].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[1].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG1_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[1].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[3].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG3_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[3].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[4].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG4_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[4].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[5].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG5_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[5].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[6].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG6_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[6].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[7].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG7_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[7].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[8].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG8_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[8].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[9].cfg_SEL                     =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG9_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[9].cfg_EN                      =       true;

    pADI_CDU_Param_List.cdu_settings[10].cfg_SEL                    =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG10_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[10].cfg_EN                     =       true;

    pADI_CDU_Param_List.cdu_settings[12].cfg_SEL                    =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG12_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[12].cfg_EN                     =       true;

    pADI_CDU_Param_List.cdu_settings[13].cfg_SEL                    =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG13_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[13].cfg_EN                     =       true;

    pADI_CDU_Param_List.cdu_settings[14].cfg_SEL                    =       (ADI_PWR_CDU_CLKIN)CFG0_BIT_CDU0_CFG14_SEL_VALUE;
    pADI_CDU_Param_List.cdu_settings[14].cfg_EN                     =       true;

    /* CGU0 Configuration*/
    pADI_CGU_Param_List.cgu0_settings.clocksettings.ctl_MSEL        =       (uint32_t)CFG0_BIT_CGU0_CTL_MSEL;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.div_CSEL        =       (uint32_t)CFG0_BIT_CGU0_DIV_CSEL;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.div_SYSSEL      =       (uint32_t)CFG0_BIT_CGU0_DIV_SYSSEL;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.div_S0SEL       =       (uint32_t)CFG0_BIT_CGU0_DIV_S0SEL;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.div_S1SEL       =       (uint32_t)CFG0_BIT_CGU0_DIV_S1SEL;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.divex_S1SELEX   =       (uint32_t)CFG0_BIT_CGU0_DIV_S1SELEX;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.div_DSEL        =       (uint32_t)CFG0_BIT_CGU0_DIV_DSEL;
    pADI_CGU_Param_List.cgu0_settings.clocksettings.div_OSEL        =       (uint32_t)CFG0_BIT_CGU0_DIV_OSEL;
    pADI_CGU_Param_List.cgu0_settings.clkin                         =       (uint32_t)CFG0_BIT_CGU0_CLKIN;
    pADI_CGU_Param_List.cgu0_settings.enable_SCLK1ExDiv             =       true;

    /* CGU1 Configuration*/
    pADI_CGU_Param_List.cgu1_settings.clocksettings.ctl_MSEL        =       (uint32_t)CFG0_BIT_CGU1_CTL_MSEL;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.div_CSEL        =       (uint32_t)CFG0_BIT_CGU1_DIV_CSEL;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.div_SYSSEL      =       (uint32_t)CFG0_BIT_CGU1_DIV_SYSSEL;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.div_S0SEL       =       (uint32_t)CFG0_BIT_CGU1_DIV_S0SEL;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.div_S1SEL       =       (uint32_t)CFG0_BIT_CGU1_DIV_S1SEL;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.divex_S0SELEX   =       (uint32_t)CFG0_BIT_CGU1_DIV_S0SELEX;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.divex_S1SELEX   =       (uint32_t)CFG0_BIT_CGU1_DIV_S1SELEX;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.div_DSEL        =       (uint32_t)CFG0_BIT_CGU1_DIV_DSEL;
    pADI_CGU_Param_List.cgu1_settings.clocksettings.div_OSEL        =       (uint32_t)CFG0_BIT_CGU1_DIV_OSEL;
    pADI_CGU_Param_List.cgu1_settings.clkin                         =       (uint32_t)CFG0_BIT_CGU1_CLKIN;
    pADI_CGU_Param_List.cgu1_settings.enable_SCLK0ExDiv             =       true;
    pADI_CGU_Param_List.cgu1_settings.enable_SCLK1ExDiv             =       true;

    pADI_CGU_Param_List.cgu1_settings.cgu1_clkinsel                 =       (ADI_PWR_CDU_CLK_SELECT)ADI_PWR_CDU_CLK_SELECT_CLKIN0;

    /* Initialize all the clocks */
    if (adi_pwr_ClockInit(&pADI_CGU_Param_List, &pADI_CDU_Param_List) != ADI_PWR_SUCCESS)
    {
       /* Return non-zero */
       status = 1u;
    }

    return status;
   }

/*@}*/
