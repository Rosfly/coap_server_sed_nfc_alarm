/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(coap);

#include "coap_utils.h"
#include "uptime.h"

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER

static const struct json_obj_descr json_uptime_get_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_uptime_get, device_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct json_uptime_get, value, JSON_TOK_NUMBER),
};

static int uptime_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	struct json_uptime_get uptime_data;
	int len;

	ARG_UNUSED(ctx);

	uptime_data.device_id = coap_device_id();
	uptime_data.value = k_uptime_get();

	json_obj_encode_buf(json_uptime_get_descr, ARRAY_SIZE(json_uptime_get_descr),
			    &uptime_data, buf, COAP_MAX_BUF_SIZE);

	len = strlen((char *)buf) + 1;

	LOG_INF("Uptime GET: %lld ms", uptime_data.value);

	return coap_resp_send(msg, msg_info, buf, len);
}

static void uptime_handler(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	coap_req_handler(ctx, msg, msg_info, NULL, uptime_handler_get);
}

static otCoapResource uptime_rsc = {
	.mUriPath = UPTIME_URI,
	.mHandler = uptime_handler,
	.mContext = NULL,
	.mNext = NULL,
};

void coap_uptime_reg_rsc(void)
{
	otInstance *ot = openthread_get_default_instance();

	LOG_INF("Registering uptime resource");
	otCoapAddResource(ot, &uptime_rsc);
}

#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */
