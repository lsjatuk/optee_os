// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2017-2018, STMicroelectronics
 */

#include <inttypes.h>
#include <drivers/stm32mp1_pwr.h>
#include <kernel/panic.h>
#include <io.h>
#include <kernel/spinlock.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <stm32_util.h>
#include <trace.h>

#include "pwr_svc.h"
#include "stm32mp1_smc.h"

struct pwr_reg_prop {
	uint32_t offset;
	uint32_t mask;
};

#define PWR_ALLOWED_MASK(_off, _mask)	{ .offset = (_off), .mask = (_mask), }

static const struct pwr_reg_prop allowed_regs[] = {
	PWR_ALLOWED_MASK(PWR_CR3_OFF, PWR_CR3_VBE | PWR_CR3_VBRS |
				      PWR_CR3_USB33DEN |
				      PWR_CR3_REG18EN | PWR_CR3_REG11EN),
	PWR_ALLOWED_MASK(PWR_WKUPCR_OFF, PWR_WKUPCR_MASK),
	PWR_ALLOWED_MASK(PWR_MPUWKUPENR_OFF, PWR_MPUWKUPENR_MASK),
};

static unsigned int lock = SPINLOCK_UNLOCK;

static uint32_t pwr_regs_lock(void)
{
	return may_spin_lock(&lock);
}

static void pwr_regs_unlock(uint32_t exceptions)
{
	may_spin_unlock(&lock, exceptions);
}

uint32_t pwr_scv_handler(uint32_t x1, uint32_t x2, uint32_t x3)
{
	uint32_t req = x1;
	uint32_t offset = x2;
	uint32_t value = x3;
	uint32_t va;
	uint32_t allowed;
	uint32_t i;
	uint32_t ret;
	uint32_t exc;

	/*
	 * Argument x2 can be either the register physical address of the
	 * register offset toward PWR_BASE.
	 */
	if ((offset & ~PWR_OFFSET_MASK) != 0) {
		if ((offset & ~PWR_OFFSET_MASK) != PWR_BASE) {
			return STM32_SIP_INVALID_PARAMS;
		}

		offset &= PWR_OFFSET_MASK;
	}

	DMSG_RAW("PWR service: %s 0x%" PRIx32 " at offset 0x%" PRIx32,
		req == STM32_SIP_REG_WRITE ? "write" :
		req == STM32_SIP_REG_SET ? "set" : "clear",
		value, offset);

	exc = pwr_regs_lock();

	for (i = 0; i < ARRAY_SIZE(allowed_regs); i++) {
		if (offset != allowed_regs[i].offset) {
			continue;
		}

		va = stm32_pwr_base() + offset;
		allowed = allowed_regs[i].mask;
		value &= allowed;

		switch (req) {
		case STM32_SIP_REG_WRITE:
			io_mask32_stm32shregs(va, value, allowed);
			FMSG("wrt off %" PRIx32 "=%" PRIx32 " => %" PRIx32,
				offset, value, read32(va));
			ret = STM32_SIP_OK;
			break;
		case STM32_SIP_REG_SET:
			io_mask32_stm32shregs(va, value, value);
			FMSG("set off %" PRIx32 "=%" PRIx32 " => %" PRIx32,
				offset, value, read32(va));
			ret = STM32_SIP_OK;
			break;
		case STM32_SIP_REG_CLEAR:
			io_mask32_stm32shregs(va, 0, value);
			FMSG("clr off %" PRIx32 "=%" PRIx32 " => %" PRIx32,
				offset, value, read32(va));
			ret = STM32_SIP_OK;
			break;
		default:
			ret = STM32_SIP_INVALID_PARAMS;
		}

		pwr_regs_unlock(exc);

		return ret;
	}

	pwr_regs_unlock(exc);

	return STM32_SIP_INVALID_PARAMS;
}
