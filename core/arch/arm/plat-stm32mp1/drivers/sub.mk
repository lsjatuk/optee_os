srcs-y 	+= stm32mp1_pwr.c
srcs-y 	+= stm32mp1_rcc.c
srcs-$(CFG_STM32_CLOCKSRC_CALIB) += stm32mp1_calib.c
srcs-y 	+= stm32_reset.c
srcs-$(CFG_STPMIC1) += stm32mp1_pmic.c
srcs-y += stm32mp1_clk.c
srcs-$(CFG_DT) += stm32mp1_clkfunc.c
srcs-y += stm32mp1_ddrc.c
