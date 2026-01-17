/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(coap);

#include "coap_utils.h"
#include "battery.h"

/*
 * ============================================================
 * DUMMY SENSOR VALUES - Adjust these for testing
 * ============================================================
 */
static int battery_percentage = BATTERY_PERCENTAGE_DEFAULT;  /* 70% */
static int battery_voltage_mv = BATTERY_VOLTAGE_MV_DEFAULT;  /* 3800 = 3.80V */

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER

/*
 * JSON descriptors for battery percentage response
 */
static const struct json_obj_descr json_battery_get_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_battery_get, device_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct json_battery_get, value, JSON_TOK_NUMBER),
};

/*
 * JSON descriptors for voltage response
 */
static const struct json_obj_descr json_voltage_get_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_voltage_get, device_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct json_voltage_get, value, JSON_TOK_NUMBER),
};

/*
 * Battery percentage GET handler
 * Returns: {"device_id":"<eui64>","value":70}
 */
static int battery_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];

	struct json_battery_get battery_data = {
		.device_id = coap_device_id(),
		.value = battery_percentage,
	};

	json_obj_encode_buf(json_battery_get_descr, ARRAY_SIZE(json_battery_get_descr),
			    &battery_data, buf, COAP_MAX_BUF_SIZE);

	LOG_INF("Battery GET: %d%%", battery_percentage);

	return coap_resp_send(msg, msg_info, buf, strlen(buf) + 1);
}

static void battery_handler(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	coap_req_handler(ctx, msg, msg_info, NULL, battery_handler_get);
}

static otCoapResource battery_rsc = {
	.mUriPath = BATTERY_URI,
	.mHandler = battery_handler,
	.mContext = NULL,
	.mNext = NULL,
};

void coap_battery_reg_rsc(void)
{
	otInstance *ot = openthread_get_default_instance();

	LOG_INF("Registering battery percentage resource");
	otCoapAddResource(ot, &battery_rsc);
}

/*
 * Voltage GET handler
 * Returns: {"device_id":"<eui64>","value":3800}
 * Value is in millivolts (3800 = 3.800V)
 */
static int voltage_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];

	struct json_voltage_get voltage_data = {
		.device_id = coap_device_id(),
		.value = battery_voltage_mv,
	};

	json_obj_encode_buf(json_voltage_get_descr, ARRAY_SIZE(json_voltage_get_descr),
			    &voltage_data, buf, COAP_MAX_BUF_SIZE);

	LOG_INF("Voltage GET: %d.%02dV", battery_voltage_mv / 1000,
		(battery_voltage_mv % 1000) / 10);

	return coap_resp_send(msg, msg_info, buf, strlen(buf) + 1);
}

static void voltage_handler(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	coap_req_handler(ctx, msg, msg_info, NULL, voltage_handler_get);
}

static otCoapResource voltage_rsc = {
	.mUriPath = VOLTAGE_URI,
	.mHandler = voltage_handler,
	.mContext = NULL,
	.mNext = NULL,
};

void coap_voltage_reg_rsc(void)
{
	otInstance *ot = openthread_get_default_instance();

	LOG_INF("Registering voltage resource");
	otCoapAddResource(ot, &voltage_rsc);
}

#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */
