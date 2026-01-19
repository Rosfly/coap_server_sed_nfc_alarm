/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COAP_UPTIME_H
#define COAP_UPTIME_H

#include <zephyr/data/json.h>

#define UPTIME_URI "uptime"

struct json_uptime_get {
	const char *device_id;
	int64_t value;  /* milliseconds since boot */
};

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER
void coap_uptime_reg_rsc(void);
#endif

#endif /* COAP_UPTIME_H */
