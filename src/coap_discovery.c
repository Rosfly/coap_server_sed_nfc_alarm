/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(coap);

#include "coap_utils.h"

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER

#define WELLKNOWN_CORE_URI ".well-known/core"

/*
 * Handler for .well-known/core resource discovery
 * Returns CoRE Link Format (RFC 6690) listing all available resources
 */
static int wellknown_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	int len = 0;

	/* Build CoRE Link Format response (compact - only rt= needed by bridge) */
	/* Format: </uri>;rt="type" */

#ifdef CONFIG_OT_COAP_SAMPLE_LED
	len += snprintf((char *)(buf + len), COAP_MAX_BUF_SIZE - len,
			"</led>;rt=\"led\",");
#endif

#ifdef CONFIG_OT_COAP_SAMPLE_SW
	len += snprintf((char *)(buf + len), COAP_MAX_BUF_SIZE - len,
			"</sw>;rt=\"button\",");
#endif

#ifdef CONFIG_OT_COAP_SAMPLE_BATTERY
	len += snprintf((char *)(buf + len), COAP_MAX_BUF_SIZE - len,
			"</battery>;rt=\"battery\",</voltage>;rt=\"voltage\",");
#endif

	/* Remove trailing comma if present */
	if (len > 0 && buf[len - 1] == ',') {
		buf[len - 1] = '\0';
		len--;
	}

	LOG_INF("Responding to .well-known/core request");

	return coap_resp_send(msg, msg_info, buf, len);
}

static void wellknown_handler(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	coap_req_handler(ctx, msg, msg_info, NULL, wellknown_handler_get);
}

static otCoapResource wellknown_rsc = {
	.mUriPath = WELLKNOWN_CORE_URI,
	.mHandler = wellknown_handler,
	.mContext = NULL,
	.mNext = NULL,
};

void coap_wellknown_reg_rsc(void)
{
	otInstance *ot = openthread_get_default_instance();

	LOG_INF("Registering .well-known/core resource");
	otCoapAddResource(ot, &wellknown_rsc);
}

#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */
