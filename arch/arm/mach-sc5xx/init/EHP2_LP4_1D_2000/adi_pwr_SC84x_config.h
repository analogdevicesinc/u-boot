#ifndef INC_ADI_PWR_SC84x_CONFIG_H_
#define INC_ADI_PWR_SC84x_CONFIG_H_

#include <adi_types.h>
#include "clock_config.h"
#if defined(__ADSPSHARCFX__)
 #include <sys/anomaly_macros_rtl.h>
#endif
#if defined(__ADSPSC835_FAMILY__) || defined(__ADSPSC846_FAMILY__)
 #if CONFIG_SHARC_CORE_CLOCK == SHARC_CORE_CLOCK_600MHZ
  #include "adi_pwr_SC846_Family_600MHz_config.h"
 #elif CONFIG_SHARC_CORE_CLOCK == SHARC_CORE_CLOCK_800MHZ
  #include "adi_pwr_SC846_Family_800MHz_config.h"
 #elif CONFIG_SHARC_CORE_CLOCK == SHARC_CORE_CLOCK_1000MHZ
  #include "adi_pwr_SC846_Family_1000MHz_config.h"
 #elif CONFIG_SHARC_CORE_CLOCK == SHARC_CORE_CLOCK_1200MHZ
  #include "adi_pwr_SC846_Family_1200MHz_config.h"
 #else
  #error Unsupported core clock configuration
 #endif
#else
 #error Unsupported processor
#endif

#if defined(__ADSPSHARCFX__)
  #include <time.h>
#elif defined(__arm__)
  #include <ADSP-SC83x.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint32_t adi_pwr_cfg0_init(void) ;
void adi_configDCLK_1(uint32_t Msel, uint32_t Dsel) ;

float32_t cclk_dclk_r(void);

#if defined(__ADSPSC835_FAMILY__)
float32_t cclk_clkin_r(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* INC_ADI_PWR_SC84x_CONFIG_H_ */
