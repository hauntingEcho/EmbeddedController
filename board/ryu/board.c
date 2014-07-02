/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/* ryu board configuration */

#include "adc.h"
#include "adc_chip.h"
#include "battery.h"
#include "charger.h"
#include "common.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "registers.h"
#include "task.h"
#include "usb_pd.h"
#include "usb_pd_config.h"
#include "util.h"

void vbus_evt(enum gpio_signal signal)
{
	ccprintf("VBUS %d, %d!\n", signal, gpio_get_level(signal));
	task_wake(TASK_ID_PD);
}

void unhandled_evt(enum gpio_signal signal)
{
	ccprintf("Unhandled INT %d,%d!\n", signal, gpio_get_level(signal));
}

void chipset_evt(enum gpio_signal signal)
{
	ccprintf("AP suspend %d,%d!\n", signal, gpio_get_level(signal));
}

#include "gpio_list.h"

/* Initialize board. */
static void board_init(void)
{
	/*
	 * Enable CC lines after all GPIO have been initialized. Note, it is
	 * important that this is enabled after the CC_DEVICE_ODL lines are
	 * set low to specify device mode.
	 */
	gpio_set_level(GPIO_USBC_CC_EN, 1);

	/* Enable interrupts on VBUS transitions. */
	gpio_enable_interrupt(GPIO_CHGR_ACOK);
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

/* Pins with alternate functions */
const struct gpio_alt_func gpio_alt_funcs[] = {
	{GPIO_B, 0x0008, 0, MODULE_USB_PD},/* SPI1: SCK(PB3) */
	{GPIO_B, 0x0002, 0, MODULE_USB_PD},/* TIM14_CH1: PB1 */
	{GPIO_B, 0x00C0, 0, MODULE_UART},  /* USART1: PB6/PB7 */
	{GPIO_A, 0xC000, 1, MODULE_UART},  /* USART2: PA14/PA15 */
	{GPIO_C, 0x0C00, 1, MODULE_UART},  /* USART3: PC10/PC11 */
	{GPIO_B, 0x0F00, 1, MODULE_I2C},   /* I2C SLAVE:PB10/11 MASTER:PB8/9 */
};
const int gpio_alt_funcs_count = ARRAY_SIZE(gpio_alt_funcs);

/* ADC channels */
const struct adc_t adc_channels[] = {
	/* Vbus sensing. Converted to mV, /10 voltage divider. */
	[ADC_VBUS] = {"VBUS",  30000, 4096, 0, STM32_AIN(0)},
	/* USB PD CC lines sensing. Converted to mV (3000mV/4096). */
	[ADC_CC1_PD] = {"CC1_PD", 3000, 4096, 0, STM32_AIN(1)},
	[ADC_CC2_PD] = {"CC1_PD", 3000, 4096, 0, STM32_AIN(3)},
	/* Charger current sensing. Converted to mA. */
	[ADC_IADP] = {"IADP",  7500, 4096, 0, STM32_AIN(8)},
	[ADC_IBAT] = {"IBAT", 37500, 4096, 0, STM32_AIN(13)},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

/* I2C ports */
const struct i2c_port_t i2c_ports[] = {
	{"master", I2C_PORT_MASTER, 100,
		GPIO_MASTER_I2C_SCL, GPIO_MASTER_I2C_SDA},
	{"slave",  I2C_PORT_SLAVE, 100,
		GPIO_SLAVE_I2C_SCL, GPIO_SLAVE_I2C_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

void board_set_usb_mux(int port, enum typec_mux mux, int polarity)
{
	/* reset everything */
	gpio_set_level(GPIO_USBC_SS_EN_L, 1);
	gpio_set_level(GPIO_USBC_DP_MODE_L, 1);
	gpio_set_level(GPIO_USBC_DP_POLARITY, 1);
	gpio_set_level(GPIO_USBC_SS1_USB_MODE_L, 1);
	gpio_set_level(GPIO_USBC_SS2_USB_MODE_L, 1);

	if (mux == TYPEC_MUX_NONE)
		/* everything is already disabled, we can return */
		return;

	if (mux == TYPEC_MUX_USB || mux == TYPEC_MUX_DOCK) {
		/* USB 3.0 uses 2 superspeed lanes */
		gpio_set_level(polarity ? GPIO_USBC_SS2_USB_MODE_L :
					  GPIO_USBC_SS1_USB_MODE_L, 0);
	}

	if (mux == TYPEC_MUX_DP || mux == TYPEC_MUX_DOCK) {
		/* DP uses available superspeed lanes (x2 or x4) */
		gpio_set_level(GPIO_USBC_DP_POLARITY, polarity);
		gpio_set_level(GPIO_USBC_DP_MODE_L, 0);
	}
	/* switch on superspeed lanes */
	gpio_set_level(GPIO_USBC_SS_EN_L, 0);
}

/* PD Port polarity as detected by the common PD code */
extern uint8_t pd_polarity;

static int command_typec(int argc, char **argv)
{
	const char * const mux_name[] = {"none", "usb", "dp", "dock"};
	enum typec_mux mux = TYPEC_MUX_NONE;
	int i;

	if (argc < 2) {
		int has_ss = !gpio_get_level(GPIO_USBC_SS_EN_L);
		int has_usb = !gpio_get_level(GPIO_USBC_SS1_USB_MODE_L) ||
				!gpio_get_level(GPIO_USBC_SS2_USB_MODE_L);
		int has_dp = !gpio_get_level(GPIO_USBC_DP_MODE_L);
		const char *dp_str = gpio_get_level(GPIO_USBC_DP_POLARITY) ?
					"DP2" : "DP1";
		const char *usb_str = gpio_get_level(GPIO_USBC_SS1_USB_MODE_L) ?
					"USB2" : "USB1";
		/* dump current state */
		ccprintf("Port CC1 %d mV  CC2 %d mV (polarity:CC%d)\n",
			pd_adc_read(0),	pd_adc_read(1), pd_polarity + 1);
		if (!has_ss)
			ccprintf("No Superspeed connection\n");
		else
			ccprintf("Superspeed %s%s%s\n",
				 has_dp ? dp_str : "",
				 has_dp && has_usb ? "+" : "",
				 has_usb ? usb_str : "");
		return EC_SUCCESS;
	}

	for (i = 0; i < ARRAY_SIZE(mux_name); i++)
		if (!strcasecmp(argv[1], mux_name[i]))
			mux = i;
	board_set_usb_mux(0, mux, pd_polarity);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(typec, command_typec,
			"[none|usb|dp|dock]",
			"Control type-C connector muxing",
			NULL);

/**
 * Discharge battery when on AC power for factory test.
 */
int board_discharge_on_ac(int enable)
{
	return charger_discharge_on_ac(enable);
}

int extpower_is_present(void)
{
	return gpio_get_level(GPIO_CHGR_ACOK);
}

/* Battery temperature ranges in degrees C */
static const struct battery_info info = {
	/* Design voltage */
	.voltage_max    = 4400,
	.voltage_normal = 3800,
	.voltage_min    = 2800,
	/* Pre-charge current: I <= 0.01C */
	.precharge_current  = 64,  /* mA */
	/* Operational temperature range */
	.start_charging_min_c = 0,
	.start_charging_max_c = 45,
	.charging_min_c       = 0,
	.charging_max_c       = 50,
	.discharging_min_c    = -20,
	.discharging_max_c    = 60,
};

const struct battery_info *battery_get_info(void)
{
	return &info;
}
