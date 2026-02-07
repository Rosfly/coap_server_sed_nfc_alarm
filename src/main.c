/*
 * Copyright (c) 2024-2025
 *
 * SPDX-License-Identifier: MIT
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
#include <zephyr/drivers/gpio.h>
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>
#include <zephyr/net/openthread.h>
#include "nfc_commission.h"

/* sw0 button for re-commissioning: hold during reset to clear dataset */
static const struct gpio_dt_spec recommission_btn =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
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
	/* Check if re-commissioning requested or device needs NFC commissioning */
	{
		otInstance *ot = openthread_get_default_instance();
		bool commissioned = false;

		if (ot) {
			/* Check if sw0 is held at boot to force re-commissioning */
			bool force_nfc = false;

			if (gpio_is_ready_dt(&recommission_btn)) {
				gpio_pin_configure_dt(&recommission_btn, GPIO_INPUT);
				k_sleep(K_MSEC(50)); /* debounce */

				if (gpio_pin_get_dt(&recommission_btn)) {
					LOG_INF("sw0 held at boot - clearing dataset for re-commissioning");

					openthread_mutex_lock();
					otThreadSetEnabled(ot, false);
					otIp6SetEnabled(ot, false);
					otInstanceErasePersistentInfo(ot);
					openthread_mutex_unlock();

					force_nfc = true;
				}
			}

			if (!force_nfc) {
				openthread_mutex_lock();
				commissioned = otDatasetIsCommissioned(ot);
				openthread_mutex_unlock();
			}

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
