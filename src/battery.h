/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COAP_BATTERY_H
#define COAP_BATTERY_H

#include <zephyr/data/json.h>

/* URI paths for resources */
#define BATTERY_URI  "battery"
#define VOLTAGE_URI  "voltage"

/*
 * ============================================================
 * DUMMY SENSOR VALUES - Adjust these constants for testing
 * ============================================================
 */
#define BATTERY_PERCENTAGE_DEFAULT  70    /* percent (0-100) */
#define BATTERY_VOLTAGE_MV_DEFAULT  3800  /* millivolts (3800 = 3.800V) */

/* JSON response structure for battery percentage */
struct json_battery_get {
	const char *device_id;
	int value;  /* percentage 0-100 */
};

/* JSON response structure for voltage */
struct json_voltage_get {
	const char *device_id;
	int value;  /* millivolts (3800 = 3.800V) */
};

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER
void coap_battery_reg_rsc(void);
void coap_voltage_reg_rsc(void);
#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */

#endif /* COAP_BATTERY_H */
