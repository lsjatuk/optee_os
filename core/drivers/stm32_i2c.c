// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (c) 2016-2018, STMicroelectronics - All Rights Reserved
 */

#include <arm32.h>
#include <drivers/stm32_i2c.h>
#include <drivers/stm32_gpio.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/dt.h>
#include <kernel/panic.h>
#include <libfdt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stm32_util.h>

/* STM32 I2C registers offsets */
#define I2C_CR1			0x00U
#define I2C_CR2			0x04U
#define I2C_OAR1		0x08U
#define I2C_OAR2		0x0CU
#define I2C_TIMINGR		0x10U
#define I2C_TIMEOUTR		0x14U
#define I2C_ISR			0x18U
#define I2C_ICR			0x1CU
#define I2C_PECR		0x20U
#define I2C_RXDR		0x24U
#define I2C_TXDR		0x28U

#define TIMINGR_CLEAR_MASK	0xF0FFFFFFU

#define MAX_NBYTE_SIZE		255U

#define I2C_NSEC_PER_SEC	1000000000L

/*
 * struct i2c_spec_s - Private I2C timing specifications.
 * @rate: I2C bus speed (Hz)
 * @rate_min: 80% of I2C bus speed (Hz)
 * @rate_max: 120% of I2C bus speed (Hz)
 * @fall_max: Max fall time of both SDA and SCL signals (ns)
 * @rise_max: Max rise time of both SDA and SCL signals (ns)
 * @hddat_min: Min data hold time (ns)
 * @vddat_max: Max data valid time (ns)
 * @sudat_min: Min data setup time (ns)
 * @l_min: Min low period of the SCL clock (ns)
 * @h_min: Min high period of the SCL clock (ns)
 */
struct i2c_spec_s {
	uint32_t rate;
	uint32_t rate_min;
	uint32_t rate_max;
	uint32_t fall_max;
	uint32_t rise_max;
	uint32_t hddat_min;
	uint32_t vddat_max;
	uint32_t sudat_min;
	uint32_t l_min;
	uint32_t h_min;
};

/*
 * struct i2c_timing_s - Private I2C output parameters.
 * @scldel: Data setup time
 * @sdadel: Data hold time
 * @sclh: SCL high period (master mode)
 * @sclh: SCL low period (master mode)
 * @is_saved: True if relating to a configuration candidate
 */
struct i2c_timing_s {
	uint8_t scldel;
	uint8_t sdadel;
	uint8_t sclh;
	uint8_t scll;
	bool is_saved;
};

/*
 * I2C specification values as per version 6.0, 4th of April 2014 [1],
 * table 10 page 48: Characteristics of the SDA and SCL bus lines for
 * Standard, Fast, and Fast-mode Plus I2C-bus devices.
 *
 * [1] https://www.nxp.com/docs/en/user-guide/UM10204.pdf
 */
static const struct i2c_spec_s i2c_specs[] = {
	[I2C_SPEED_STANDARD] = {
		.rate = STANDARD_RATE,
		.rate_min = (STANDARD_RATE * 80) / 100,
		.rate_max = (STANDARD_RATE * 120) / 100,
		.fall_max = 300,
		.rise_max = 1000,
		.hddat_min = 0,
		.vddat_max = 3450,
		.sudat_min = 250,
		.l_min = 4700,
		.h_min = 4000,
	},
	[I2C_SPEED_FAST] = {
		.rate = FAST_RATE,
		.rate_min = (FAST_RATE * 80) / 100,
		.rate_max = (FAST_RATE * 120) / 100,
		.fall_max = 300,
		.rise_max = 300,
		.hddat_min = 0,
		.vddat_max = 900,
		.sudat_min = 100,
		.l_min = 1300,
		.h_min = 600,
	},
	[I2C_SPEED_FAST_PLUS] = {
		.rate = FAST_PLUS_RATE,
		.rate_min = (FAST_PLUS_RATE * 80) / 100,
		.rate_max = (FAST_PLUS_RATE * 120) / 100,
		.fall_max = 100,
		.rise_max = 120,
		.hddat_min = 0,
		.vddat_max = 450,
		.sudat_min = 50,
		.l_min = 500,
		.h_min = 260,
	},
};

static int i2c_request_memory_write(struct i2c_handle_s *hi2c,
				    uint16_t dev_addr, uint16_t mem_addr,
				    uint16_t mem_add_size,
				    uint64_t tick_to, uint64_t tick_start);
static int i2c_request_memory_read(struct i2c_handle_s *hi2c, uint16_t dev_addr,
				   uint16_t mem_addr, uint16_t mem_add_size,
				   uint64_t tick_to, uint64_t tick_start);

/* Private functions to handle flags during polling transfer */
static int i2c_wait_flag(struct i2c_handle_s *hi2c, uint32_t flag,
			 uint8_t awaited_value,
			 uint64_t tick_to, uint64_t tick_start);
static int i2c_wait_txis(struct i2c_handle_s *hi2c,
			 uint64_t tick_to, uint64_t tick_start);
static int i2c_wait_stop(struct i2c_handle_s *hi2c,
			 uint64_t tick_to, uint64_t tick_start);
static int i2c_ack_failed(struct i2c_handle_s *hi2c,
			  uint64_t tick_to, uint64_t tick_start);

/* Private function to flush TXDR register */
static void i2c_flush_txdr(struct i2c_handle_s *hi2c);

/* Private function to start, restart or stop a transfer */
static void i2c_transfer_config(struct i2c_handle_s *hi2c, uint16_t dev_addr,
				uint16_t size, uint32_t i2c_mode,
				uint32_t request);

static uintptr_t get_base(struct i2c_handle_s *hi2c)
{
	if (!cpu_mmu_enabled())
		return hi2c->pbase;

	return hi2c->vbase;
}

static bool i2c_is_secure(struct i2c_handle_s *hi2c)
{
	return hi2c->dt_status == DT_STATUS_OK_SEC;
}

static uint64_t ms2tick(uint32_t timeout_ms)
{
	return ((uint64_t)timeout_ms * read_cntfrq()) / 1000;
}

static uint64_t timeout_start(void)
{
	return read_cntpct();
}

static bool timeout_elapsed(uint64_t tick_start, uint64_t tick_to)
{
	return (tick_to != 0U) && ((read_cntpct() - tick_start) > tick_to);
}

static void notif_i2c_timeout(struct i2c_handle_s *hi2c)
{
	hi2c->i2c_err |= I2C_ERROR_TIMEOUT;
	hi2c->i2c_mode = I2C_MODE_NONE;
	hi2c->i2c_state = I2C_STATE_READY;
}

static void save_cfg(struct i2c_handle_s *hi2c, struct i2c_cfg *cfg)
{
	uintptr_t base = get_base(hi2c);

	stm32_clock_enable(hi2c->clock);

	cfg->cr1 = mmio_read_32(base + I2C_CR1);
	cfg->cr2 = mmio_read_32(base + I2C_CR2);
	cfg->oar1 = mmio_read_32(base + I2C_OAR1);
	cfg->oar2 = mmio_read_32(base + I2C_OAR2);
	cfg->timingr = mmio_read_32(base + I2C_TIMINGR);

	stm32_clock_disable(hi2c->clock);
}

static void restore_cfg(struct i2c_handle_s *hi2c, struct i2c_cfg *cfg)
{
	uintptr_t base = get_base(hi2c);

	if (hi2c->lock) {
		panic();
	}

	stm32_clock_enable(hi2c->clock);

	mmio_clrbits_32(base + I2C_CR1, I2C_CR1_PE);
	mmio_write_32(base + I2C_TIMINGR, cfg->timingr & TIMINGR_CLEAR_MASK);
	mmio_write_32(base + I2C_OAR1, cfg->oar1);
	mmio_write_32(base + I2C_CR2, cfg->cr2);
	mmio_write_32(base + I2C_OAR2, cfg->oar2);
	mmio_write_32(base + I2C_CR1, cfg->cr1 & ~I2C_CR1_PE);
	mmio_setbits_32(base + I2C_CR1, cfg->cr1 & I2C_CR1_PE);

	stm32_clock_disable(hi2c->clock);
}

static void __maybe_unused dump_cfg(struct i2c_cfg *cfg __maybe_unused)
{
	DMSG("CR1:  %x", (unsigned)cfg->cr1);
	DMSG("CR2:  %x", (unsigned)cfg->cr2);
	DMSG("OAR1: %x", (unsigned)cfg->oar1);
	DMSG("OAR2: %x", (unsigned)cfg->oar2);
	DMSG("TIM:  %x", (unsigned)cfg->timingr);
}

static void __maybe_unused dump_i2c(struct i2c_handle_s *hi2c)
{
	uintptr_t __maybe_unused base = get_base(hi2c);

	stm32_clock_enable(hi2c->clock);

	DMSG("CR1:  %x", (unsigned)mmio_read_32(base + I2C_CR1));
	DMSG("CR2:  %x", (unsigned)mmio_read_32(base + I2C_CR2));
	DMSG("OAR1: %x", (unsigned)mmio_read_32(base + I2C_OAR1));
	DMSG("OAR2: %x", (unsigned)mmio_read_32(base + I2C_OAR2));
	DMSG("TIM:  %x", (unsigned)mmio_read_32(base + I2C_TIMINGR));

	stm32_clock_disable(hi2c->clock);
}

/*
 * @brief  Compute the I2C device timings.
 * @param  init: Ref to the initialization configuration structure
 * @param  clock_src: I2C clock source frequency (Hz)
 * @param  timing: Pointer to the final computed timing result
 * @retval 0 if OK, negative value else
 */
static int i2c_compute_timing(struct stm32_i2c_init_s *init,
			      uint32_t clock_src, uint32_t *timing)
{
	enum i2c_speed_e mode = init->speed_mode;
	uint32_t speed_freq;
	uint32_t i2cclk = UDIV_ROUND_NEAREST(I2C_NSEC_PER_SEC, clock_src);
	uint32_t i2cbus;
	uint32_t p_prev = I2C_TIMINGR_PRESC_MAX;
	uint32_t af_delay_min;
	uint32_t af_delay_max;
	uint32_t dnf_delay;
	uint32_t tsync;
	uint32_t clk_min;
	uint32_t clk_max;
	int clk_error_prev;
	uint16_t p;
	uint16_t l;
	uint16_t a;
	uint16_t h;
	unsigned int sdadel_min;
	unsigned int sdadel_max;
	unsigned int scldel_min;
	unsigned int delay;
	int s = -1;
	struct i2c_timing_s solutions[I2C_TIMINGR_PRESC_MAX];

	switch (mode) {
	case I2C_SPEED_STANDARD:
	case I2C_SPEED_FAST:
	case I2C_SPEED_FAST_PLUS:
		break;
	default:
		EMSG("I2C speed out of bound {%d/%d}\n",
		     mode, I2C_SPEED_FAST_PLUS);
		return -1;
	}

	speed_freq = i2c_specs[mode].rate;
	i2cbus = UDIV_ROUND_NEAREST(I2C_NSEC_PER_SEC, speed_freq);
	clk_error_prev = INT_MAX;

	if ((init->rise_time > i2c_specs[mode].rise_max) ||
	    (init->fall_time > i2c_specs[mode].fall_max)) {
		EMSG(" I2C timings out of bound Rise{%d>%d}/Fall{%d>%d}\n",
		     init->rise_time, i2c_specs[mode].rise_max,
		     init->fall_time, i2c_specs[mode].fall_max);
		return -1;
	}

	if (init->digital_filter_coef > STM32_I2C_DIGITAL_FILTER_MAX) {
		EMSG("DNF out of bound %d/%d\n",
		     init->digital_filter_coef, STM32_I2C_DIGITAL_FILTER_MAX);
		return -1;
	}

	/*  Analog and Digital Filters */
	af_delay_min = (init->analog_filter ?
			STM32_I2C_ANALOG_FILTER_DELAY_MIN : 0);
	af_delay_max = (init->analog_filter ?
			STM32_I2C_ANALOG_FILTER_DELAY_MAX : 0);
	dnf_delay = init->digital_filter_coef * i2cclk;

	sdadel_min = i2c_specs[mode].hddat_min + init->fall_time;
	delay = af_delay_min - ((init->digital_filter_coef + 3) * i2cclk);
	if (SUB_OVERFLOW(sdadel_min, delay ,&sdadel_min))
		sdadel_min = 0;

	sdadel_max = i2c_specs[mode].vddat_max - init->rise_time;
	delay = af_delay_max - ((init->digital_filter_coef + 4) * i2cclk);
	if (SUB_OVERFLOW(sdadel_max, delay ,&sdadel_max))
		sdadel_max = 0;

	scldel_min = init->rise_time + i2c_specs[mode].sudat_min;

	DMSG("I2C SDADEL(min/max): %u/%u, SCLDEL(Min): %u\n",
	     sdadel_min, sdadel_max, scldel_min);

	memset(&solutions, 0, sizeof(solutions));

	/* Compute possible values for PRESC, SCLDEL and SDADEL */
	for (p = 0; p < I2C_TIMINGR_PRESC_MAX; p++) {
		for (l = 0; l < I2C_TIMINGR_SCLDEL_MAX; l++) {
			uint32_t scldel = (l + 1) * (p + 1) * i2cclk;

			if (scldel < scldel_min) {
				continue;
			}

			for (a = 0; a < I2C_TIMINGR_SDADEL_MAX; a++) {
				uint32_t sdadel = (a * (p + 1) + 1) * i2cclk;

				if ((sdadel >= sdadel_min) &&
				    (sdadel <= sdadel_max) &&
				    (p != p_prev)) {
					solutions[p].scldel = l;
					solutions[p].sdadel = a;
					solutions[p].is_saved = true;
					p_prev = p;
					break;
				}
			}

			if (p_prev == p) {
				break;
			}
		}
	}

	if (p_prev == I2C_TIMINGR_PRESC_MAX) {
		EMSG(" I2C no Prescaler solution\n");
		return -1;
	}

	tsync = af_delay_min + dnf_delay + (2 * i2cclk);
	clk_max = I2C_NSEC_PER_SEC / i2c_specs[mode].rate_min;
	clk_min = I2C_NSEC_PER_SEC / i2c_specs[mode].rate_max;

	/*
	 * Among prescaler possibilities discovered above figures out SCL Low
	 * and High Period. Provided:
	 * - SCL Low Period has to be higher than Low Period of the SCL Clock
	 *   defined by I2C Specification. I2C Clock has to be lower than
	 *   (SCL Low Period - Analog/Digital filters) / 4.
	 * - SCL High Period has to be lower than High Period of the SCL Clock
	 *   defined by I2C Specification.
	 * - I2C Clock has to be lower than SCL High Period.
	 */
	for (p = 0; p < I2C_TIMINGR_PRESC_MAX; p++) {
		uint32_t prescaler = (p + 1) * i2cclk;

		if (!solutions[p].is_saved) {
			continue;
		}

		for (l = 0; l < I2C_TIMINGR_SCLL_MAX; l++) {
			uint32_t tscl_l = ((l + 1) * prescaler) + tsync;

			if ((tscl_l < i2c_specs[mode].l_min) ||
			    (i2cclk >=
			     ((tscl_l - af_delay_min - dnf_delay) / 4))) {
				continue;
			}

			for (h = 0; h < I2C_TIMINGR_SCLH_MAX; h++) {
				uint32_t tscl_h = ((h + 1) * prescaler) + tsync;
				uint32_t tscl = tscl_l + tscl_h +
						init->rise_time +
						init->fall_time;

				if ((tscl >= clk_min) && (tscl <= clk_max) &&
				    (tscl_h >= i2c_specs[mode].h_min) &&
				    (i2cclk < tscl_h)) {
					int clk_error = tscl - i2cbus;

					if (clk_error < 0) {
						clk_error = -clk_error;
					}

					if (clk_error < clk_error_prev) {
						clk_error_prev = clk_error;
						solutions[p].scll = l;
						solutions[p].sclh = h;
						s = p;
					}
				}
			}
		}
	}

	if (s < 0) {
		EMSG(" I2C no solution at all\n");
		return -1;
	}

	/* Finalize timing settings */
	*timing = I2C_SET_TIMINGR_PRESC(s) |
		   I2C_SET_TIMINGR_SCLDEL(solutions[s].scldel) |
		   I2C_SET_TIMINGR_SDADEL(solutions[s].sdadel) |
		   I2C_SET_TIMINGR_SCLH(solutions[s].sclh) |
		   I2C_SET_TIMINGR_SCLL(solutions[s].scll);

	DMSG("I2C TIMINGR (PRESC/SCLDEL/SDADEL): %i/%i/%i\n",
		s, solutions[s].scldel, solutions[s].sdadel);
	DMSG("I2C TIMINGR (SCLH/SCLL): %i/%i\n",
		solutions[s].sclh, solutions[s].scll);
	DMSG("I2C TIMINGR: 0x%x\n", *timing);

	return 0;
}

/*
 * @brief  Setup the I2C device timings.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  init: Ref to the initialization configuration structure
 * @param  timing: Pointer to the final computed timing result
 * @retval 0 if OK, negative value else
 */
static int i2c_setup_timing(struct i2c_handle_s *hi2c,
			    struct stm32_i2c_init_s *init,
			    uint32_t *timing)
{
	int rc = 0;
	uint32_t clock_src;

	clock_src = stm32_clock_get_rate(hi2c->clock);
	if (clock_src == 0U) {
		EMSG("I2C clock rate is 0\n");
		return -1;
	}

	do {
		rc = i2c_compute_timing(init, clock_src, timing);
		if (rc != 0) {
			EMSG("Failed to compute I2C timings\n");
			if (init->speed_mode > I2C_SPEED_STANDARD) {
				init->speed_mode--;
				IMSG("Downgrade I2C speed to %uHz)\n",
				     i2c_specs[init->speed_mode].rate);
			} else {
				break;
			}
		}
	} while (rc != 0);

	if (rc != 0) {
		EMSG("Impossible to compute I2C timings\n");
		return rc;
	}

	DMSG("I2C Speed Mode(%i), Freq(%i), Clk Source(%i)\n",
	     init->speed_mode, i2c_specs[init->speed_mode].rate, clock_src);
	DMSG("I2C Rise(%i) and Fall(%i) Time\n",
	     init->rise_time, init->fall_time);
	DMSG("I2C Analog Filter(%s), DNF(%i)\n",
	     (init->analog_filter ? "On" : "Off"), init->digital_filter_coef);

	return 0;
}

/*
 * @brief  Configure I2C Analog noise filter.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C peripheral.
 * @param  analog_filter: New state of the Analog filter
 * @retval 0 if OK, negative value else
 */
static int i2c_config_analog_filter(struct i2c_handle_s *hi2c,
				    uint32_t analog_filter)
{
	uintptr_t base = get_base(hi2c);

	if ((hi2c->i2c_state != I2C_STATE_READY) || (hi2c->lock != 0U)) {
		return -1;
	}

	hi2c->lock = 1;

	hi2c->i2c_state = I2C_STATE_BUSY;

	/* Disable the selected I2C peripheral */
	mmio_clrbits_32(base + I2C_CR1, I2C_CR1_PE);

	/* Reset I2Cx ANOFF bit */
	mmio_clrbits_32(base + I2C_CR1, I2C_CR1_ANFOFF);

	/* Set analog filter bit*/
	mmio_setbits_32(base + I2C_CR1, analog_filter);

	/* Enable the selected I2C peripheral */
	mmio_setbits_32(base + I2C_CR1, I2C_CR1_PE);

	hi2c->i2c_state = I2C_STATE_READY;

	hi2c->lock = 0;

	return 0;
}

/*
 * @brief  Get I2C setup information from the device tree and set pinctrl
 *         configuration.
 * @param  fdt: Pointer to the device tree
 * @param  node: I2C node offset
 * @param  init: Ref to the initialization configuration structure
 * @retval 0 if OK, negative value else
 */
int stm32_i2c_get_setup_from_fdt(void *fdt, int node,
				 struct stm32_i2c_init_s *init,
				 struct stm32_pinctrl **pinctrl,
				 size_t *pinctrl_count)
{
	const fdt32_t *cuint;
	int count;

	cuint = fdt_getprop(fdt, node, "i2c-scl-rising-time-ns", NULL);
	if (cuint == NULL) {
		init->rise_time = STM32_I2C_RISE_TIME_DEFAULT;
	} else {
		init->rise_time = fdt32_to_cpu(*cuint);
	}

	cuint = fdt_getprop(fdt, node, "i2c-scl-falling-time-ns", NULL);
	if (cuint == NULL) {
		init->fall_time = STM32_I2C_FALL_TIME_DEFAULT;
	} else {
		init->fall_time = fdt32_to_cpu(*cuint);
	}

	cuint = fdt_getprop(fdt, node, "clock-frequency", NULL);
	if (cuint == NULL) {
		init->speed_mode = STM32_I2C_SPEED_DEFAULT;
	} else {
		switch (fdt32_to_cpu(*cuint)) {
		case STANDARD_RATE:
			init->speed_mode = I2C_SPEED_STANDARD;
			break;
		case FAST_RATE:
			init->speed_mode = I2C_SPEED_FAST;
			break;
		case FAST_PLUS_RATE:
			init->speed_mode = I2C_SPEED_FAST_PLUS;
			break;
		default:
			init->speed_mode = STM32_I2C_SPEED_DEFAULT;
			break;
		}
	}

	count = stm32_pinctrl_fdt_get_pinctrl(fdt, node, NULL, 0);
	if (count <= 0) {
		*pinctrl = NULL;
		*pinctrl_count = 0;
		return count;
	}

	if (count > 2) {
		panic("Too many PINCTRLs found");
	}

	*pinctrl = calloc(count, sizeof(**pinctrl));
	if (!*pinctrl) {
		panic();
	}

	*pinctrl_count = stm32_pinctrl_fdt_get_pinctrl(fdt, node,
						       *pinctrl, count);
	assert(*pinctrl_count == (unsigned int)count);

	return 0;
}

/*
 * @brief  Initialize the I2C device.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  init_data: Ref to the initialization configuration structure
 * @retval 0 if OK, negative value else
 */
int stm32_i2c_init(struct i2c_handle_s *hi2c,
		   struct stm32_i2c_init_s *init_data)
{
	int rc = 0;
	uint32_t timing;
	uintptr_t base = get_base(hi2c);

	if (hi2c == NULL) {
		return -1;
	}

	if (hi2c->i2c_state == I2C_STATE_RESET) {
		hi2c->lock = 0;
	}

	hi2c->i2c_state = I2C_STATE_BUSY;

	rc = i2c_setup_timing(hi2c, init_data, &timing);
	if (rc != 0) {
		return rc;
	}

	stm32_clock_enable(hi2c->clock);

	/* Disable the selected I2C peripheral */
	mmio_clrbits_32(base + I2C_CR1, I2C_CR1_PE);

	/* Configure I2Cx: Frequency range */
	mmio_write_32(base + I2C_TIMINGR, timing & TIMINGR_CLEAR_MASK);

	/* Disable Own Address1 before set the Own Address1 configuration */
	mmio_clrbits_32(base + I2C_OAR1, I2C_OAR1_OA1EN);

	/* Configure I2Cx: Own Address1 and ack own address1 mode */
	if (init_data->addressing_mode == I2C_ADDRESSINGMODE_7BIT) {
		mmio_write_32(base + I2C_OAR1,
			      I2C_OAR1_OA1EN | init_data->own_address1);
	} else { /* I2C_ADDRESSINGMODE_10BIT */
		mmio_write_32(base + I2C_OAR1,
			      I2C_OAR1_OA1EN | I2C_OAR1_OA1MODE |
			      init_data->own_address1);
	}

	mmio_write_32(base + I2C_CR2, 0);

	/* Configure I2Cx: Addressing Master mode */
	if (init_data->addressing_mode == I2C_ADDRESSINGMODE_10BIT) {
		mmio_setbits_32(base + I2C_CR2, I2C_CR2_ADD10);
	}

	/*
	 * Enable the AUTOEND by default, and enable NACK
	 * (should be disabled only during Slave process).
	 */
	mmio_setbits_32(base + I2C_CR2, I2C_CR2_AUTOEND | I2C_CR2_NACK);

	/* Disable Own Address2 before set the Own Address2 configuration */
	mmio_clrbits_32(base + I2C_OAR2, I2C_DUALADDRESS_ENABLE);

	/* Configure I2Cx: Dual mode and Own Address2 */
	mmio_write_32(base + I2C_OAR2,
		      init_data->dual_address_mode |
		      init_data->own_address2 |
		      (init_data->own_address2_masks << 8));

	/* Configure I2Cx: Generalcall and NoStretch mode */
	mmio_write_32(base + I2C_CR1,
		      init_data->general_call_mode |
		      init_data->no_stretch_mode);

	/* Enable the selected I2C peripheral */
	mmio_setbits_32(base + I2C_CR1, I2C_CR1_PE);

	hi2c->i2c_err = I2C_ERROR_NONE;
	hi2c->i2c_state = I2C_STATE_READY;
	hi2c->i2c_mode = I2C_MODE_NONE;

	rc = i2c_config_analog_filter(hi2c, init_data->analog_filter ?
						I2C_ANALOGFILTER_ENABLE :
						I2C_ANALOGFILTER_DISABLE);
	if (rc != 0) {
		EMSG("Cannot initialize I2C analog filter (%d)\n", rc);
		stm32_clock_disable(hi2c->clock);
		return rc;
	}

	stm32_clock_disable(hi2c->clock);

	return rc;
}

/*
 * @brief  Generic function to write an amount of data in blocking mode
 *         (for Memory Mode and Master Mode)
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  mem_addr: Internal memory address (if Memory Mode)
 * @param  mem_add_size: Size of internal memory address (if Memory Mode)
 * @param  p_data: Pointer to data buffer
 * @param  size: Amount of data to be sent
 * @param  timeout_ms: Timeout duration in milliseconds
 * @param  mode: Communication mode
 * @retval 0 if OK, negative value else
 */
static int i2c_write(struct i2c_handle_s *hi2c, uint16_t dev_addr,
		     uint16_t mem_addr, uint16_t mem_add_size,
		     uint8_t *p_data, uint16_t size, uint32_t timeout_ms,
		     enum i2c_mode_e mode)
{
	uint64_t tick_start;
	uintptr_t base = get_base(hi2c);
	int rc = -1;
	uint64_t tick_to = ms2tick(timeout_ms);
	uint8_t *p_buff = p_data;
	size_t xfer_size;
	size_t xfer_count = size;

	if ((mode != I2C_MODE_MASTER) && (mode != I2C_MODE_MEM)) {
		return -1;
	}

	if ((hi2c->i2c_state != I2C_STATE_READY) || (hi2c->lock != 0U)) {
		return -1;
	}

	if ((p_data == NULL) || (size == 0U)) {
		return -1;
	}

	stm32_clock_enable(hi2c->clock);

	hi2c->lock = 1;

	tick_start = timeout_start();

	if (i2c_wait_flag(hi2c, I2C_FLAG_BUSY, 1, ms2tick(I2C_TIMEOUT_BUSY_MS),
			  tick_start) != 0) {
		goto bail;
	}

	hi2c->i2c_state = I2C_STATE_BUSY_TX;
	hi2c->i2c_mode = mode;
	hi2c->i2c_err = I2C_ERROR_NONE;

	if (mode == I2C_MODE_MEM) {
		/* In Memory Mode, Send Slave Address and Memory Address */
		if (i2c_request_memory_write(hi2c, dev_addr,
					     mem_addr, mem_add_size,
					     tick_to, tick_start) != 0) {
			goto bail;
		}

		if (xfer_count > MAX_NBYTE_SIZE) {
			xfer_size = MAX_NBYTE_SIZE;
			i2c_transfer_config(hi2c, dev_addr, xfer_size,
					    I2C_RELOAD_MODE, I2C_NO_STARTSTOP);
		} else {
			xfer_size = xfer_count;
			i2c_transfer_config(hi2c, dev_addr, xfer_size,
					    I2C_AUTOEND_MODE, I2C_NO_STARTSTOP);
		}
	} else {
		/* In Master Mode, Send Slave Address */
		if (xfer_count > MAX_NBYTE_SIZE) {
			xfer_size = MAX_NBYTE_SIZE;
			i2c_transfer_config(hi2c, dev_addr, xfer_size,
					    I2C_RELOAD_MODE,
					    I2C_GENERATE_START_WRITE);
		} else {
			xfer_size = xfer_count;
			i2c_transfer_config(hi2c, dev_addr, xfer_size,
					    I2C_AUTOEND_MODE,
					    I2C_GENERATE_START_WRITE);
		}
	}

	do {
		if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
			goto bail;
		}

		mmio_write_8(base + I2C_TXDR, *p_buff);
		p_buff++;
		xfer_count--;
		xfer_size--;

		if ((xfer_count != 0U) && (xfer_size == 0U)) {
			/* Wait until TCR flag is set */
			if (i2c_wait_flag(hi2c, I2C_FLAG_TCR, 0, tick_to,
					  tick_start) != 0) {
				goto bail;
			}

			if (xfer_count > MAX_NBYTE_SIZE) {
				xfer_size = MAX_NBYTE_SIZE;
				i2c_transfer_config(hi2c, dev_addr,
						    xfer_size,
						    I2C_RELOAD_MODE,
						    I2C_NO_STARTSTOP);
			} else {
				xfer_size = xfer_count;
				i2c_transfer_config(hi2c, dev_addr,
						    xfer_size,
						    I2C_AUTOEND_MODE,
						    I2C_NO_STARTSTOP);
			}
		}

	} while (xfer_count > 0U);

	/*
	 * No need to Check TC flag, with AUTOEND mode the stop
	 * is automatically generated.
	 * Wait until STOPF flag is reset.
	 */
	if (i2c_wait_stop(hi2c, tick_to, tick_start) != 0) {
		goto bail;
	}

	mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);

	mmio_clrbits_32(base + I2C_CR2, I2C_RESET_CR2);

	hi2c->i2c_state = I2C_STATE_READY;
	hi2c->i2c_mode = I2C_MODE_NONE;

	rc = 0;

bail:
	hi2c->lock = 0;
	stm32_clock_disable(hi2c->clock);

	return rc;
}

/*
 * @brief  Write an amount of data in blocking mode to a specific memory
 *         address.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  mem_addr: Internal memory address
 * @param  mem_add_size: Size of internal memory address
 * @param  p_data: Pointer to data buffer
 * @param  size: Amount of data to be sent
 * @param  timeout_ms: Timeout duration in milliseconds
 * @retval 0 if OK, negative value else
 */
int stm32_i2c_mem_write(struct i2c_handle_s *hi2c, uint16_t dev_addr,
			uint16_t mem_addr, uint16_t mem_add_size,
			uint8_t *p_data, uint16_t size, uint32_t timeout_ms)
{
	return i2c_write(hi2c, dev_addr, mem_addr, mem_add_size,
			 p_data, size, timeout_ms, I2C_MODE_MEM);
}

/*
 * @brief  Transmits in master mode an amount of data in blocking mode.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  p_data: Pointer to data buffer
 * @param  size: Amount of data to be sent
 * @param  timeout_ms: Timeout duration in milliseconds
 * @retval 0 if OK, negative value else
 */
int stm32_i2c_master_transmit(struct i2c_handle_s *hi2c, uint16_t dev_addr,
			      uint8_t *p_data, uint16_t size,
			      uint32_t timeout_ms)
{
	return i2c_write(hi2c, dev_addr, 0, 0,
			 p_data, size, timeout_ms, I2C_MODE_MASTER);
}

/*
 * Optimized 1 byte read/write function for unpaged sequences.
 * 8-bit addressing mode / single byte transferred / use default I2C timeout.
 * 'unpg88' refers to 8bit address/8bit value.
 */
int stm32_i2c_read_write_membyte(struct i2c_handle_s *hi2c, uint16_t dev_addr,
			      unsigned int mem_addr, uint8_t *p_data,
			      bool write)
{
	uint64_t tick_start;
	uintptr_t base = get_base(hi2c);
	int rc = 1;
	uint64_t tick_to = ms2tick(I2C_TIMEOUT_BUSY_MS);
	uint8_t *p_buff = p_data;

	if ((hi2c->i2c_state != I2C_STATE_READY) ||
	    (hi2c->lock != 0U) ||
	    (p_data == NULL)) {
		return 1;
	}

	stm32_clock_enable(hi2c->clock);

	hi2c->lock = 1;

	tick_start = timeout_start();
	if (i2c_wait_flag(hi2c, I2C_FLAG_BUSY, 1, tick_to, tick_start) != 0) {
		goto bail;
	}

	hi2c->i2c_state = write ? I2C_STATE_BUSY_TX : I2C_STATE_BUSY_RX;
	hi2c->i2c_mode = I2C_MODE_MEM;
	hi2c->i2c_err = I2C_ERROR_NONE;

	i2c_transfer_config(hi2c, dev_addr, I2C_MEMADD_SIZE_8BIT,
			    write ? I2C_RELOAD_MODE : I2C_SOFTEND_MODE,
			    I2C_GENERATE_START_WRITE);

	tick_start = timeout_start();
	if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
		goto bail;
	}

	mmio_write_8(base + I2C_TXDR, (uint8_t)(mem_addr & 0x0FFU));

	tick_start = timeout_start();
	if (i2c_wait_flag(hi2c, write ? I2C_FLAG_TCR : I2C_FLAG_TC, 0,
			  tick_to, tick_start) != 0) {
		goto bail;
	}

	i2c_transfer_config(hi2c, dev_addr, I2C_MEMADD_SIZE_8BIT,
			    I2C_AUTOEND_MODE,
			    write ? I2C_NO_STARTSTOP : I2C_GENERATE_START_READ);

	tick_start = timeout_start();
	if (write) {
		if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
			goto bail;
		}
		mmio_write_8(base + I2C_TXDR, *p_buff);

	} else {
		if (i2c_wait_flag(hi2c, I2C_FLAG_RXNE, 0,
				  tick_to, tick_start) != 0) {
			goto bail;
		}
		*p_buff = mmio_read_8(base + I2C_RXDR);
	}

	tick_start = timeout_start();
	if (i2c_wait_stop(hi2c, tick_to, tick_start) != 0) {
		goto bail;
	}

	mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);
	mmio_clrbits_32(base + I2C_CR2, I2C_RESET_CR2);

	hi2c->i2c_state = I2C_STATE_READY;
	hi2c->i2c_mode = I2C_MODE_NONE;

	rc = 0;

bail:
	hi2c->lock = 0;
	stm32_clock_disable(hi2c->clock);

	return rc;
}

/*
 * @brief  Generic function to read an amount of data in blocking mode
 *         (for Memory Mode and Master Mode)
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  mem_addr: Internal memory address (if Memory Mode)
 * @param  mem_add_size: Size of internal memory address (if Memory Mode)
 * @param  p_data: Pointer to data buffer
 * @param  size: Amount of data to be sent
 * @param  timeout_ms: Timeout duration in milliseconds
 * @param  mode: Communication mode
 * @retval 0 if OK, negative value else
 */
static int i2c_read(struct i2c_handle_s *hi2c, uint16_t dev_addr,
		    uint16_t mem_addr, uint16_t mem_add_size,
		    uint8_t *p_data, uint16_t size, uint32_t timeout_ms,
		    enum i2c_mode_e mode)
{
	uintptr_t base = get_base(hi2c);
	uint64_t tick_start;
	int rc = -1;
	uint64_t tick_to = ms2tick(timeout_ms);
	uint8_t *p_buff = p_data;
	size_t xfer_count = size;
	size_t xfer_size;

	if ((mode != I2C_MODE_MASTER) && (mode != I2C_MODE_MEM)) {
		return -1;
	}

	if ((hi2c->i2c_state != I2C_STATE_READY) || (hi2c->lock != 0U)) {
		return -1;
	}

	if ((p_data == NULL) || (size == 0U)) {
		return  -1;
	}

	stm32_clock_enable(hi2c->clock);

	hi2c->lock = 1;

	tick_start = timeout_start();
	if (i2c_wait_flag(hi2c, I2C_FLAG_BUSY, 1, ms2tick(I2C_TIMEOUT_BUSY_MS),
			  tick_start) != 0) {
		goto bail;
	}

	hi2c->i2c_state = I2C_STATE_BUSY_RX;
	hi2c->i2c_mode = mode;
	hi2c->i2c_err = I2C_ERROR_NONE;

	if (mode == I2C_MODE_MEM) {
		/* Send Memory Address */
		if (i2c_request_memory_read(hi2c, dev_addr,
					    mem_addr, mem_add_size,
					    tick_to, tick_start) != 0) {
			goto bail;
		}
	}

	/*
	 * Send Slave Address.
	 * Set NBYTES to write and reload if xfer_count > MAX_NBYTE_SIZE
	 * and generate RESTART.
	 */
	if (xfer_count > MAX_NBYTE_SIZE) {
		xfer_size = MAX_NBYTE_SIZE;
		i2c_transfer_config(hi2c, dev_addr, xfer_size,
				    I2C_RELOAD_MODE, I2C_GENERATE_START_READ);
	} else {
		xfer_size = xfer_count;
		i2c_transfer_config(hi2c, dev_addr, xfer_size,
				    I2C_AUTOEND_MODE, I2C_GENERATE_START_READ);
	}

	do {
		if (i2c_wait_flag(hi2c, I2C_FLAG_RXNE, 0, tick_to,
				  tick_start) != 0) {
			goto bail;
		}

		*p_buff = mmio_read_8(base + I2C_RXDR);
		p_buff++;
		xfer_size--;
		xfer_count--;

		if ((xfer_count != 0U) && (xfer_size == 0U)) {
			if (i2c_wait_flag(hi2c, I2C_FLAG_TCR, 0, tick_to,
					  tick_start) != 0) {
				goto bail;
			}

			if (xfer_count > MAX_NBYTE_SIZE) {
				xfer_size = MAX_NBYTE_SIZE;
				i2c_transfer_config(hi2c, dev_addr,
						    xfer_size,
						    I2C_RELOAD_MODE,
						    I2C_NO_STARTSTOP);
			} else {
				xfer_size = xfer_count;
				i2c_transfer_config(hi2c, dev_addr,
						    xfer_size,
						    I2C_AUTOEND_MODE,
						    I2C_NO_STARTSTOP);
			}
		}
	} while (xfer_count > 0U);

	/*
	 * No need to Check TC flag, with AUTOEND mode the stop
	 * is automatically generated.
	 * Wait until STOPF flag is reset.
	 */
	if (i2c_wait_stop(hi2c, tick_to, tick_start) != 0) {
		goto bail;
	}

	mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);

	mmio_clrbits_32(base + I2C_CR2, I2C_RESET_CR2);

	hi2c->i2c_state = I2C_STATE_READY;
	hi2c->i2c_mode = I2C_MODE_NONE;

	rc = 0;

bail:
	hi2c->lock = 0;
	stm32_clock_disable(hi2c->clock);

	return rc;
}

/*
 * @brief  Read an amount of data in blocking mode from a specific memory
 *	   address.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  mem_addr: Internal memory address
 * @param  mem_add_size: Size of internal memory address
 * @param  p_data: Pointer to data buffer
 * @param  size: Amount of data to be sent
 * @param  timeout_ms: Timeout duration in milliseconds
 * @retval 0 if OK, negative value else
 */
int stm32_i2c_mem_read(struct i2c_handle_s *hi2c, uint16_t dev_addr,
		       uint16_t mem_addr, uint16_t mem_add_size,
		       uint8_t *p_data, uint16_t size, uint32_t timeout_ms)
{
	return i2c_read(hi2c, dev_addr, mem_addr, mem_add_size,
			p_data, size, timeout_ms, I2C_MODE_MEM);
}

/*
 * @brief  Receives in master mode an amount of data in blocking mode.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  p_data: Pointer to data buffer
 * @param  size: Amount of data to be sent
 * @param  timeout_ms: Timeout duration in milliseconds
 * @retval 0 if OK, negative value else
 */
int stm32_i2c_master_receive(struct i2c_handle_s *hi2c, uint16_t dev_addr,
			     uint8_t *p_data, uint16_t size,
			     uint32_t timeout_ms)
{
	return i2c_read(hi2c, dev_addr, 0, 0,
			p_data, size, timeout_ms, I2C_MODE_MASTER);
}

/*
 * @brief  Checks if target device is ready for communication.
 * @note   This function is used with Memory devices
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  trials: Number of trials
 * @param  timeout_ms: Timeout duration in milliseconds
 * @retval True if device is ready, false else
 */
bool stm32_i2c_is_device_ready(struct i2c_handle_s *hi2c,
			       uint16_t dev_addr, uint32_t trials,
			       uint32_t timeout_ms)
{
	uintptr_t base = get_base(hi2c);
	uint32_t i2c_trials = 0U;
	bool rc = false;
	uint64_t tick_to = ms2tick(timeout_ms);

	if ((hi2c->i2c_state != I2C_STATE_READY) || (hi2c->lock != 0U)) {
		return rc;
	}

	stm32_clock_enable(hi2c->clock);

	hi2c->lock = 1;
	hi2c->i2c_mode = I2C_MODE_NONE;

	if ((mmio_read_32(base + I2C_ISR) & I2C_FLAG_BUSY) != 0U) {
		goto bail;
	}

	hi2c->i2c_state = I2C_STATE_BUSY;
	hi2c->i2c_err = I2C_ERROR_NONE;

	do {
		uint64_t tick_start;
		uint32_t isr;

		/* Generate Start */
		if ((mmio_read_32(base + I2C_OAR1) & I2C_OAR1_OA1MODE) == 0) {
			mmio_write_32(base + I2C_CR2,
				      (((uint32_t)dev_addr & I2C_CR2_SADD) |
				       I2C_CR2_START | I2C_CR2_AUTOEND) &
				      ~I2C_CR2_RD_WRN);
		} else {
			mmio_write_32(base + I2C_CR2,
				      (((uint32_t)dev_addr & I2C_CR2_SADD) |
				       I2C_CR2_START | I2C_CR2_ADD10) &
				      ~I2C_CR2_RD_WRN);
		}

		/*
		 * No need to Check TC flag, with AUTOEND mode the stop
		 * is automatically generated.
		 * Wait until STOPF flag is set or a NACK flag is set.
		 */
		tick_start = timeout_start();
		do {
			if (timeout_elapsed(tick_start, tick_to)) {
				notif_i2c_timeout(hi2c);
				goto bail;
			}

			isr = mmio_read_32(base + I2C_ISR);
		} while ((isr & (I2C_FLAG_STOPF | I2C_FLAG_AF)) == 0);

		if ((mmio_read_32(base + I2C_ISR) & I2C_FLAG_AF) == 0U) {
			if (i2c_wait_flag(hi2c, I2C_FLAG_STOPF, 0, tick_to,
					  tick_start) != 0) {
				goto bail;
			}

			mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);

			hi2c->i2c_state = I2C_STATE_READY;

			rc = true;
			goto bail;
		}

		if (i2c_wait_flag(hi2c, I2C_FLAG_STOPF, 0, tick_to,
				  tick_start) != 0) {
			goto bail;
		}

		mmio_write_32(base + I2C_ICR, I2C_FLAG_AF);

		mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);

		if (i2c_trials == trials) {
			mmio_setbits_32(base + I2C_CR2, I2C_CR2_STOP);

			if (i2c_wait_flag(hi2c, I2C_FLAG_STOPF, 0, tick_to,
					  tick_start) != 0) {
				goto bail;
			}

			mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);
		}

		i2c_trials++;
	} while (i2c_trials < trials);

	notif_i2c_timeout(hi2c);

bail:
#ifdef STM32_I2C_MAYBE_NON_SECURE
	if (rc) {
		/* Save the active secure configuraton */
		save_cfg(hi2c, &hi2c->sec_cfg);
	}
#endif

	hi2c->lock = 0;
	stm32_clock_disable(hi2c->clock);

	return rc;
}

/*
 * @brief  Master sends target device address followed by internal memory
 *	   address for write request.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  mem_addr: Internal memory address
 * @param  mem_add_size: Size of internal memory address
 * @param  tick_to: Tick timeout duration
 * @param  tick_start: Tick start value
 * @retval 0 if OK, negative value else
 */
static int i2c_request_memory_write(struct i2c_handle_s *hi2c,
				    uint16_t dev_addr, uint16_t mem_addr,
				    uint16_t mem_add_size, uint64_t tick_to,
				    uint64_t tick_start)
{
	uintptr_t base = get_base(hi2c);

	i2c_transfer_config(hi2c, dev_addr, mem_add_size, I2C_RELOAD_MODE,
			    I2C_GENERATE_START_WRITE);

	if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
		return -1;
	}

	if (mem_add_size == I2C_MEMADD_SIZE_8BIT) {
		/* Send Memory Address */
		mmio_write_8(base + I2C_TXDR,
			     (uint8_t)(mem_addr & 0x00FFU));
	} else {
		/* Send MSB of Memory Address */
		mmio_write_8(base + I2C_TXDR,
			     (uint8_t)((mem_addr & 0xFF00U) >> 8));

		if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
			return -1;
		}

		/* Send LSB of Memory Address */
		mmio_write_8(base + I2C_TXDR,
			     (uint8_t)(mem_addr & 0x00FFU));
	}

	if (i2c_wait_flag(hi2c, I2C_FLAG_TCR, 0, tick_to, tick_start) != 0) {
		return -1;
	}

	return 0;
}

/*
 * @brief  Master sends target device address followed by internal memory
 *	   address for read request.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  dev_addr: Target device address
 * @param  mem_addr: Internal memory address
 * @param  mem_add_size: Size of internal memory address
 * @param  tick_to: Tick timeout duration
 * @param  tick_start: Tick start value
 * @retval 0 if OK, negative value else
 */
static int i2c_request_memory_read(struct i2c_handle_s *hi2c, uint16_t dev_addr,
				   uint16_t mem_addr, uint16_t mem_add_size,
				   uint64_t tick_to, uint64_t tick_start)
{
	uintptr_t base = get_base(hi2c);

	i2c_transfer_config(hi2c, dev_addr, mem_add_size, I2C_SOFTEND_MODE,
			    I2C_GENERATE_START_WRITE);

	if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
		return -1;
	}

	if (mem_add_size == I2C_MEMADD_SIZE_8BIT) {
		/* Send Memory Address */
		mmio_write_8(base + I2C_TXDR,
			     (uint8_t)(mem_addr & 0x00FFU));
	} else {
		/* Send MSB of Memory Address */
		mmio_write_8(base + I2C_TXDR,
			     (uint8_t)((mem_addr & 0xFF00U) >> 8));

		if (i2c_wait_txis(hi2c, tick_to, tick_start) != 0) {
			return -1;
		}

		/* Send LSB of Memory Address */
		mmio_write_8(base + I2C_TXDR,
			     (uint8_t)(mem_addr & 0x00FFU));
	}

	if (i2c_wait_flag(hi2c, I2C_FLAG_TC, 0, tick_to, tick_start) != 0) {
		return -1;
	}

	return 0;
}

/*
 * @brief  I2C Tx data register flush process.
 * @param  hi2c: I2C handle
 * @retval None
 */
static void i2c_flush_txdr(struct i2c_handle_s *hi2c)
{
	uintptr_t base = get_base(hi2c);

	/*
	 * If a pending TXIS flag is set,
	 * write a dummy data in TXDR to clear it.
	 */
	if ((mmio_read_32(base + I2C_ISR) & I2C_FLAG_TXIS) != 0U) {
		mmio_write_32(base + I2C_TXDR, 0);
	}

	/* Flush TX register if not empty */
	if ((mmio_read_32(base + I2C_ISR) & I2C_FLAG_TXE) == 0U) {
		mmio_setbits_32(base + I2C_ISR, I2C_FLAG_TXE);
	}
}

/*
 * @brief  This function handles I2C Communication timeout.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  flag: Specifies the I2C flag to check
 * @param  awaited_value: The awaited bit value for the flag (0 or 1)
 * @param  tick_to: Tick timeout duration
 * @param  tick_start: Tick start value
 * @retval 0 if OK, negative value else
 */
static int i2c_wait_flag(struct i2c_handle_s *hi2c, uint32_t flag,
			 uint8_t awaited_value, uint64_t tick_to,
			 uint64_t tick_start)
{
	for ( ; ; ) {
		uint32_t isr = read32(get_base(hi2c) + I2C_ISR);

		if (!!(isr & flag) != !!awaited_value) {
			return 0;
		}

		if (timeout_elapsed(tick_start, tick_to)) {
			notif_i2c_timeout(hi2c);
			hi2c->lock = 0;

			return -1;
		}
	}
}

/*
 * @brief  This function handles I2C Communication timeout for specific usage
 *	   of TXIS flag.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  tick_to: Tick timeout duration
 * @param  tick_start: Tick start value
 * @retval 0 if OK, negative value else
 */
static int i2c_wait_txis(struct i2c_handle_s *hi2c, uint64_t tick_to,
			 uint64_t tick_start)
{
	while ((read32(get_base(hi2c) + I2C_ISR) & I2C_FLAG_TXIS) == 0U) {
		if (i2c_ack_failed(hi2c, tick_to, tick_start) != 0) {
			return -1;
		}

		if (timeout_elapsed(tick_start, tick_to)) {
			notif_i2c_timeout(hi2c);
			hi2c->lock = 0;

			return -1;
		}
	}

	return 0;
}

/*
 * @brief  This function handles I2C Communication timeout for specific
 *	   usage of STOP flag.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  tick_to: Tick timeout duration
 * @param  tick_start: Tick start value
 * @retval 0 if OK, negative value else
 */
static int i2c_wait_stop(struct i2c_handle_s *hi2c, uint64_t tick_to,
			 uint64_t tick_start)
{
	while ((read32(get_base(hi2c) + I2C_ISR) & I2C_FLAG_STOPF) == 0U) {
		if (i2c_ack_failed(hi2c, tick_to, tick_start) != 0) {
			return -1;
		}

		if (timeout_elapsed(tick_start, tick_to)) {
			notif_i2c_timeout(hi2c);
			hi2c->lock = 0;

			return -1;
		}
	}

	return 0;
}

/*
 * @brief  This function handles Acknowledge failed detection during
 *	   an I2C Communication.
 * @param  hi2c: Pointer to a struct i2c_handle_s structure that contains
 *               the configuration information for the specified I2C.
 * @param  tick_to: Tick timeout duration
 * @param  tick_start: Tick start value
 * @retval 0 if OK, negative value else
 */
static int i2c_ack_failed(struct i2c_handle_s *hi2c, uint64_t tick_to,
			  uint64_t tick_start)
{
	uintptr_t base = get_base(hi2c);

	if ((mmio_read_32(base + I2C_ISR) & I2C_FLAG_AF) == 0U) {
		return 0;
	}

	/*
	 * Wait until STOP Flag is reset.
	 * AutoEnd should be initiate after AF.
	 */
	while ((mmio_read_32(base + I2C_ISR) & I2C_FLAG_STOPF) == 0U) {
		if (timeout_elapsed(tick_start, tick_to)) {
			notif_i2c_timeout(hi2c);
			hi2c->lock = 0;

			return -1;
		}
	}

	mmio_write_32(base + I2C_ICR, I2C_FLAG_AF);

	mmio_write_32(base + I2C_ICR, I2C_FLAG_STOPF);

	i2c_flush_txdr(hi2c);

	mmio_clrbits_32(base + I2C_CR2, I2C_RESET_CR2);

	hi2c->i2c_err |= I2C_ERROR_AF;
	hi2c->i2c_state = I2C_STATE_READY;
	hi2c->i2c_mode = I2C_MODE_NONE;

	hi2c->lock = 0;

	return -1;
}

/*
 * @brief  Handles I2Cx communication when starting transfer or during transfer
 *	   (TC or TCR flag are set).
 * @param  hi2c: I2C handle
 * @param  dev_addr: Specifies the slave address to be programmed
 * @param  size: Specifies the number of bytes to be programmed.
 *   This parameter must be a value between 0 and 255.
 * @param  i2c_mode: New state of the I2C START condition generation.
 *   This parameter can be one of the following values:
 *     @arg @ref I2C_RELOAD_MODE: Enable Reload mode.
 *     @arg @ref I2C_AUTOEND_MODE: Enable Automatic end mode.
 *     @arg @ref I2C_SOFTEND_MODE: Enable Software end mode.
 * @param  request: New state of the I2C START condition generation.
 *   This parameter can be one of the following values:
 *     @arg @ref I2C_NO_STARTSTOP: Don't Generate stop and start condition.
 *     @arg @ref I2C_GENERATE_STOP: Generate stop condition
 *                                  (size should be set to 0).
 *     @arg @ref I2C_GENERATE_START_READ: Generate Restart for read request.
 *     @arg @ref I2C_GENERATE_START_WRITE: Generate Restart for write request.
 * @retval None
 */
static void i2c_transfer_config(struct i2c_handle_s *hi2c, uint16_t dev_addr,
				uint16_t size, uint32_t i2c_mode,
				uint32_t request)
{
	uint32_t clr_value, set_value;

	clr_value = (I2C_CR2_SADD | I2C_CR2_NBYTES | I2C_CR2_RELOAD |
		     I2C_CR2_AUTOEND | I2C_CR2_START | I2C_CR2_STOP) |
		(I2C_CR2_RD_WRN & (request >> (31U - I2C_CR2_RD_WRN_OFFSET)));

	set_value = ((uint32_t)dev_addr & I2C_CR2_SADD) |
		(((uint32_t)size << I2C_CR2_NBYTES_OFFSET) & I2C_CR2_NBYTES) |
		i2c_mode | request;

	mmio_clrsetbits_32(get_base(hi2c) + I2C_CR2, clr_value, set_value);
}

#ifdef STM32_I2C_MAYBE_NON_SECURE
/*
 * Secure side needs the PMIC. If it is secure, it can be freely used.
 * If the PMIC is non secure, the configuration must be save/restored
 * when used by the secure side.
 */
#endif

void stm32_i2c_resume(struct i2c_handle_s *hi2c)
{
	if (hi2c->i2c_state == I2C_STATE_READY) {
		return;
	}

	if ((hi2c->i2c_state != I2C_STATE_RESET) &&
	    (hi2c->i2c_state != I2C_STATE_SUSPENDED)) {
		panic();
	}

#ifdef STM32_I2C_MAYBE_NON_SECURE
	if (!i2c_is_secure(hi2c)) {
		stm32_pinctrl_store_standby_cfg(hi2c->pinctrl,
						hi2c->pinctrl_count);
		save_cfg(hi2c, &hi2c->alt_cfg);
	}
#endif

	stm32_pinctrl_load_active_cfg(hi2c->pinctrl, hi2c->pinctrl_count);

	if (hi2c->i2c_state == I2C_STATE_RESET) {
		/* This is no valid I2C configuration loaded yet */
		return;
	}

	restore_cfg(hi2c, &hi2c->sec_cfg);

	hi2c->i2c_state = I2C_STATE_READY;
}

void stm32_i2c_suspend(struct i2c_handle_s *hi2c)
{
	if ((hi2c->i2c_state == I2C_STATE_SUSPENDED)) {
		return;
	}

	if ((hi2c->i2c_state != I2C_STATE_READY)) {
		panic();
	}

	save_cfg(hi2c, &hi2c->sec_cfg);

	stm32_pinctrl_load_standby_cfg(hi2c->pinctrl,
				       hi2c->pinctrl_count);

#ifdef STM32_I2C_MAYBE_NON_SECURE
	if (!i2c_is_secure(hi2c)) {
		restore_cfg(hi2c, &hi2c->alt_cfg);
	}
#endif

	hi2c->i2c_state = I2C_STATE_SUSPENDED;
}
