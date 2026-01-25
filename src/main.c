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

#ifdef CONFIG_OT_COAP_SAMPLE_UPTIME
#include "uptime.h"
#endif /* CONFIG_OT_COAP_SAMPLE_UPTIME */

#ifdef CONFIG_OT_COAP_NFC_COMMISSION
#include <openthread/dataset.h>
#include <zephyr/net/openthread.h>
#include "nfc_commission.h"
#endif /* CONFIG_OT_COAP_NFC_COMMISSION */


int main(void)
{
	int ret;

	/* Initialize CoAP first - this starts OpenThread instance */
	ret = coap_init();
	if (ret) {
		return ret;
	}

#ifdef CONFIG_OT_COAP_NFC_COMMISSION
	/* Check if device needs NFC commissioning */
	{
		otInstance *ot = openthread_get_default_instance();

		if (ot) {
			openthread_mutex_lock();
			bool commissioned = otDatasetIsCommissioned(ot);
			openthread_mutex_unlock();

			if (!commissioned) {
				LOG_INF("Device not commissioned - starting NFC mode");

				ret = nfc_commission_init();
				if (ret) {
					LOG_ERR("NFC init failed: %d", ret);
					/* Continue anyway - device won't join network */
				} else {
					ret = nfc_commission_start();
					if (ret == 0) {
						LOG_INF("NFC commissioning complete");
					} else {
						LOG_ERR("NFC commissioning failed: %d", ret);
					}
					nfc_commission_stop();
				}
			} else {
				LOG_INF("Device already commissioned - skipping NFC");
			}
		}
	}
#endif /* CONFIG_OT_COAP_NFC_COMMISSION */

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
#ifdef CONFIG_OT_COAP_SAMPLE_UPTIME
	coap_uptime_reg_rsc();
#endif /* CONFIG_OT_COAP_SAMPLE_UPTIME */

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
