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
#include "button.h"
#endif /* CONFIG_OT_COAP_SAMPLE_SW */

#ifdef CONFIG_OT_COAP_SAMPLE_BATTERY
#include "battery.h"
#endif /* CONFIG_OT_COAP_SAMPLE_BATTERY */


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

	return 0;
}
