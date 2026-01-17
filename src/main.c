/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap);

#include <coap_utils.h>
#include "coap_discovery.h"
#include "network_monitor.h"

#ifdef CONFIG_OT_COAP_SAMPLE_LED
#include "led.h"
#endif /* CONFIG_OT_COAP_SAMPLE_LED */

#ifdef CONFIG_OT_COAP_SAMPLE_SW
#include <zephyr/drivers/gpio.h>
#include "button.h"
#endif /* CONFIG_OT_COAP_SAMPLE_SW */

#ifdef CONFIG_OT_COAP_SAMPLE_BATTERY
#include "battery.h"
#endif /* CONFIG_OT_COAP_SAMPLE_BATTERY */

#ifdef CONFIG_OT_COAP_SAMPLE_SW
/*
 * Get button configuration from the devicetree sw0 alias. This is mandatory.
 */
#define SW0_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});
static struct gpio_callback button_cb_data;

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	coap_led_set_state("ff03::1", 0, LED_MSG_STATE_TOGGLE);
}
#endif /* CONFIG_OT_COAP_SAMPLE_SW */

int main(void)
{
	int ret;

	/* Initialize CoAP first - this starts OpenThread instance */
	ret = coap_init();
	if (ret) {
		return ret;
	}

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER
	/* Register CoAP resources AFTER CoAP initialization */
#ifdef CONFIG_OT_COAP_SAMPLE_LED
	coap_led_reg_rsc();
#endif /* CONFIG_OT_COAP_SAMPLE_LED */
#ifdef CONFIG_OT_COAP_SAMPLE_SW
	coap_btn_reg_rsc();
#endif /* CONFIG_OT_COAP_SAMPLE_SW */
#ifdef CONFIG_OT_COAP_SAMPLE_BATTERY
	coap_battery_reg_rsc();
	coap_voltage_reg_rsc();
#endif /* CONFIG_OT_COAP_SAMPLE_BATTERY */

	/* Register .well-known/core resource discovery last */
	coap_wellknown_reg_rsc();

	/* Start network state monitoring for automatic reconnection */
	ret = network_monitor_start();
	if (ret) {
		LOG_ERR("Failed to start network monitor: %d", ret);
		/* Non-fatal - continue anyway */
	}
#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */

#ifdef CONFIG_OT_COAP_SAMPLE_SW
	button_init(&button);

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback_dt(&button, &button_cb_data);
#endif /*CONFIG_OT_COAP_SAMPLE_SW */

	return 0;
}
