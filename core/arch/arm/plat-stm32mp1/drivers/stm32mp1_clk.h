/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 */

#ifndef __STM32MP1_CLK_H__
#define __STM32MP1_CLK_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum stm32mp_osc_id {
	_HSI,
	_HSE,
	_CSI,
	_LSI,
	_LSE,
	_I2S_CKIN,
	_USB_PHY_48,
	NB_OSC,
	_UNKNOWN_OSC_ID = 0xFF
};

void __stm32mp1_clk_enable(unsigned long id, bool caller_is_secure);
void __stm32mp1_clk_disable(unsigned long id, bool caller_is_secure);
bool stm32mp1_clk_is_enabled(unsigned long id);

static inline void stm32mp1_clk_enable_non_secure(unsigned long id)
{
	__stm32mp1_clk_enable(id, false);
}

static inline void stm32mp1_clk_enable_secure(unsigned long id)
{
	__stm32mp1_clk_enable(id, true);
}

static inline void stm32mp1_clk_disable_non_secure(unsigned long id)
{
	__stm32mp1_clk_disable(id, false);
}

static inline void stm32mp1_clk_disable_secure(unsigned long id)
{
	__stm32mp1_clk_disable(id, true);
}

unsigned int stm32mp1_clk_get_refcount(unsigned long id);

unsigned long stm32mp1_clk_get_rate(unsigned long id);

unsigned long stm32mp1_clk_rcc2id(size_t offset, size_t bit);

void stm32mp_register_clock_parents_secure(unsigned long id);

void stm32mp_update_earlyboot_clocks_state(void);

void stm32mp1_clock_suspend(void);
void stm32mp1_clock_resume(void);

#endif /* __STM32MP1_CLK_H__ */
