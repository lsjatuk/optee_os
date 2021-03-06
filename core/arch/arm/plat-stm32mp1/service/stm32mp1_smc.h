/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2016-2018, STMicroelectronics
 * Copyright (c) 2018, Linaro Limited
 */
#ifndef __STM32MP1_SMC_H__
#define __STM32MP1_SMC_H__

/*
 * SIP Functions
 */
#define STM32_SIP_SVC_VERSION_MAJOR		0x0
#define STM32_SIP_SVC_VERSION_MINOR		0x1

/* SIP service generic return codes */
#define STM32_SIP_OK				0x0
#define STM32_SIP_NOT_SUPPORTED			0xffffffffU
#define STM32_SIP_FAILED			0xfffffffeU
#define STM32_SIP_INVALID_PARAMS		0xfffffffdU

/*
 * SMC function IDs for STM32 Service queries
 * STM32 SMC services use the space between 0x82000000 and 0x8200FFFF
 * like this is defined in SMC calling Convention by ARM
 * for SiP (silicon Partner)
 * http://infocenter.arm.com/help/topic/com.arm.doc.den0028a/index.html
 */

/*
 * SIP function STM32_SIP_FUNC_CALL_COUNT
 *
 * Argument a0: (input) SMCC ID
 *		(output) dummy value 0
 */
#define STM32_SIP_FUNC_CALL_COUNT		0xff00

/*
 * SIP function STM32_SIP_FUNC_UID
 *
 * Argument a0: (input) SMCC ID
 *		(output) Lowest 32bit of the stm32mp1 SIP service UUID
 * Argument a1: (output) Next 32bit of the stm32mp1 SIP service UUID
 * Argument a2: (output) Next 32bit of the stm32mp1 SIP service UUID
 * Argument a3: (output) Last 32bit of the stm32mp1 SIP service UUID
 */
#define STM32_SIP_FUNC_UID			0xff01

/*
 * SIP function STM32_SIP_FUNC_VERSION
 *
 * Argument a0: (input) SMCC ID
 *		(output) STM32 SIP service major
 * Argument a1: (output) STM32 SIP service minor
 */
#define STM32_SIP_FUNC_VERSION			0xff03

/*
 * SIP function STM32_SIP_FUNC_RCC
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (input) Service ID (STM32_SIP_REG_xxx)
 * Argument a2: (input) register offset or physical address
 *		(output) register read value, if applicable
 * Argument a3: (input) register target value if applicable
 */
#define STM32_SIP_FUNC_RCC			0x1000

/*
 * SIP function STM32_SIP_FUNC_PWR
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (input) Service ID (STM32_SIP_REG_xxx)
 * Argument a2: (input) register offset or physical address
 *		(output) register read value, if applicable
 * Argument a3: (input) register target value if applicable
 */
#define STM32_SIP_FUNC_PWR			0x1001

/* Service ID for STM32_SIP_FUNC_RCC/_PWR */
#define STM32_SIP_REG_READ			0x0
#define STM32_SIP_REG_WRITE			0x1
#define STM32_SIP_REG_SET			0x2
#define STM32_SIP_REG_CLEAR			0x3

/*
 * SIP functions STM32_SIP_FUNC_BSEC
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (input) Clock ID (from DT clock bindings)
 */
#define STM32_SIP_RCC_CAL			0x1002

/*
 * SIP functions STM32_SIP_FUNC_BSEC
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (input) Service ID (STM32_SIP_BSEC_xxx)
 * Argument a2: (input) OTP index
 *		(output) OTP read value, if applicable
 * Argument a3: (input) OTP value if applicable
 */
#define STM32_SIP_FUNC_BSEC			0x1003

/* Service ID for STM32_SIP_FUNC_BSEC */
#define STM32_SIP_BSEC_READ_SHADOW		0x1
#define STM32_SIP_BSEC_PROG_OTP			0x2
#define STM32_SIP_BSEC_WRITE_SHADOW		0x3
#define STM32_SIP_BSEC_READ_OTP			0x4

/*
 * SIP functions STM32_SIP_FUNC_SR_MODE
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (unused)
 * Argument a2: (input) Target selfrefresh mode
 */
#define STM32_SIP_FUNC_SR_MODE			0x1004

/* DDR Self-Refresh modes */
#define STM32_SIP_SR_MODE_SSR			0x0
#define STM32_SIP_SR_MODE_ASR			0x1
#define STM32_SIP_SR_MODE_HSR			0x2

/*
 * SIP functions STM32_SIP_FUNC_CSTOP
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (unused)
 * Argument a2: (unused)
 * Argument a3: (input) Target SoC mode
 */
#define STM32_SIP_FUNC_CSTOP			0x1005

/* Valid SoC modes used for CSTOP, */
#define STM32_SIP_CSLEEP_RUN			0x0
#define STM32_SIP_CSTOP_ALLOW_STOP		0x1
#define STM32_SIP_CSTOP_ALLOW_LP_STOP		0x2
#define STM32_SIP_CSTOP_ALLOW_LPLV_STOP		0x3
#define STM32_SIP_CSTOP_ALLOW_STANDBY		0x4
#define STM32_SIP_CSTOP_ALLOW_STANDBY_DDR_OFF	0x5
#define STM32_SIP_CSTOP_SHUTDOWN		0x6

/*
 * SIP functions STM32_SIP_FUNC_STANDBY
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a1: (unused)
 * Argument a2: (unused)
 * Argument a3: (input) non null only for DDR off standby
 */
#define STM32_SIP_FUNC_STANDBY			0x1006

/*
 * SIP function STM32_SIP_FUNC_SHUTDOWN
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 */
#define STM32_SIP_FUNC_SHUTDOWN			0x1007

/*
 * SIP function STM32_SIP_FUNC_PD_DOMAIN
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a2: (index) ID of target power domain to be enabled/disabled
 * Argument a3: (input) 0 to disable, 1 to eanble target domain
 */
#define STM32_SIP_FUNC_PD_DOMAIN		0x1008

/* Valid IDs for power domain for function STM32_SIP_FUNC_PD_DOMAIN */
#define STM32_SIP_PD_VSW			0x0
#define STM32_SIP_PD_CORE_RET			0x1
#define STM32_SIP_PD_CORE			0x2
#define STM32_SIP_PD_MAX_PM_DOMAIN		0x3

/*
 * OEM Functions
 */
#define STM32_OEM_SVC_VERSION_MAJOR		0x0
#define STM32_OEM_SVC_VERSION_MINOR		0x1

/* OEM service generic return codes */
#define STM32_OEM_OK				0x0
#define STM32_OEM_NOT_SUPPORTED			0xffffffffU
#define STM32_OEM_FAILED			0xfffffffeU
#define STM32_OEM_INVALID_PARAMS		0xfffffffdU

/*
 * OEM function STM32_OEM_FUNC_LP_FORCE_PARAMS
 *
 * Argument a0: (input) SMCC ID
 *		(output) status return code
 * Argument a2: (input) ID of the mode: suspend or shutdown (off)
 * Argument a3: (input) ID of the power state to be reached for the mode
 *                      Refer to stm32mp1 power bindings.
 */
#define STM32_OEM_FUNC_LP_FORCE_PARAMS		0x0f800

/* Valid IDs for power mode configured with STM32_OEM_FUNC_LP_FORCE_PARAMS */
#define STM32_OEM_LP_FORCE_SUSPEND_PARAMS	0
#define STM32_OEM_LP_FORCE_OFF_PARAMS		1

#endif /* __STM32MP1_SMC_H__*/
