/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2017-2018, STMicroelectronics - All Rights Reserved
 */

#ifndef __STM32MP1_PMIC_H__
#define __STM32MP1_PMIC_H__

#include <kernel/panic.h>

#ifdef CFG_STPMIC1
void stm32mp_pmic_apply_boot_on_config(void);
void stm32mp_pmic_apply_lp_config(const char *lp_state);
void stm32mp_get_pmic(void);
void stm32mp_put_pmic(void);
#else
static inline void stm32mp_pmic_apply_boot_on_config(void)
{
}
static inline void stm32mp_pmic_apply_lp_config(const char *lp_state __unused)
{
}
static inline void stm32mp_get_pmic(void)
{
	panic();
}
static inline void stm32mp_put_pmic(void)
{
	panic();
}
#endif

#endif /*__STM32MP1_PMIC_H__*/
