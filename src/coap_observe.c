/*
 * Copyright (c) 2024-2025
 *
 * SPDX-License-Identifier: MIT
 *
 * CoAP Observe (RFC 7641) support for push notifications
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(coap_observe, LOG_LEVEL_INF);

#include "coap_observe.h"
#include "coap_utils.h"

void coap_observe_init(struct coap_observe_resource *resource, const char *uri_path)
{
	memset(resource, 0, sizeof(*resource));
	resource->uri_path = uri_path;
	resource->observer_count = 0;
}

void coap_observe_clear(struct coap_observe_resource *resource)
{
	for (int i = 0; i < COAP_MAX_OBSERVERS; i++) {
		resource->observers[i].active = false;
	}
	resource->observer_count = 0;
	LOG_INF("Cleared all observers for %s", resource->uri_path);
}

static struct coap_observer *find_observer(struct coap_observe_resource *resource,
					   const otMessageInfo *msg_info)
{
	for (int i = 0; i < COAP_MAX_OBSERVERS; i++) {
		struct coap_observer *obs = &resource->observers[i];
		if (obs->active &&
		    otIp6IsAddressEqual(&obs->peer_info.mPeerAddr, &msg_info->mPeerAddr) &&
		    obs->peer_info.mPeerPort == msg_info->mPeerPort) {
			return obs;
		}
	}
	return NULL;
}

static struct coap_observer *alloc_observer(struct coap_observe_resource *resource)
{
	for (int i = 0; i < COAP_MAX_OBSERVERS; i++) {
		if (!resource->observers[i].active) {
			return &resource->observers[i];
		}
	}
	return NULL;
}

static int register_observer(struct coap_observe_resource *resource,
			     otMessage *msg, const otMessageInfo *msg_info)
{
	struct coap_observer *obs;
	uint8_t token_len;
	const uint8_t *token;

	/* Check if already registered */
	obs = find_observer(resource, msg_info);
	if (obs) {
		/* Update token */
		token = otCoapMessageGetToken(msg);
		token_len = otCoapMessageGetTokenLength(msg);
		memcpy(obs->token, token, token_len);
		obs->token_len = token_len;
		LOG_DBG("Updated observer for %s", resource->uri_path);
		return 0;
	}

	/* Allocate new observer */
	obs = alloc_observer(resource);
	if (!obs) {
		LOG_WRN("No free observer slots for %s", resource->uri_path);
		return -ENOMEM;
	}

	/* Store observer info */
	obs->active = true;
	memcpy(&obs->peer_info, msg_info, sizeof(otMessageInfo));
	/* Swap src/dst for response */
	obs->peer_info.mPeerAddr = msg_info->mPeerAddr;
	obs->peer_info.mPeerPort = msg_info->mPeerPort;

	token = otCoapMessageGetToken(msg);
	token_len = otCoapMessageGetTokenLength(msg);
	memcpy(obs->token, token, token_len);
	obs->token_len = token_len;
	obs->observe_seq = 0;
	obs->last_notify = k_uptime_get();

	resource->observer_count++;

	LOG_INF("Registered observer for %s (total: %d)",
		resource->uri_path, resource->observer_count);

	return 0;
}

static int deregister_observer(struct coap_observe_resource *resource,
			       const otMessageInfo *msg_info)
{
	struct coap_observer *obs = find_observer(resource, msg_info);

	if (obs) {
		obs->active = false;
		resource->observer_count--;
		LOG_INF("Deregistered observer for %s (remaining: %d)",
			resource->uri_path, resource->observer_count);
		return 0;
	}

	return -ENOENT;
}

/**
 * Get the Observe option value from a CoAP message
 * Returns 0 on success, -ENOENT if no Observe option
 */
static int get_observe_option(otMessage *msg, uint32_t *observe_value)
{
	const otCoapOption *option;
	otCoapOptionIterator iterator;
	otError err;
	uint64_t value;

	err = otCoapOptionIteratorInit(&iterator, msg);
	if (err != OT_ERROR_NONE) {
		LOG_DBG("Option iterator init failed: %d", err);
		return -EINVAL;
	}

	/* Find Observe option (option number 6) */
	option = otCoapOptionIteratorGetFirstOptionMatching(&iterator, OT_COAP_OPTION_OBSERVE);
	if (option == NULL) {
		/* No Observe option - this is normal for regular GET requests */
		return -ENOENT;
	}

	/* Use OpenThread's built-in uint value extraction */
	err = otCoapOptionIteratorGetOptionUintValue(&iterator, &value);
	if (err != OT_ERROR_NONE) {
		LOG_WRN("Failed to get Observe option value: %d", err);
		return -EINVAL;
	}

	*observe_value = (uint32_t)value;
	LOG_DBG("Observe option value: %u", *observe_value);

	return 0;
}

int coap_observe_handle(struct coap_observe_resource *resource,
			otMessage *msg, const otMessageInfo *msg_info,
			uint32_t *observe_seq)
{
	uint32_t observe_value;
	int ret;

	/* Check for Observe option */
	ret = get_observe_option(msg, &observe_value);
	if (ret != 0) {
		/* No Observe option - regular GET request */
		return -ENOENT;
	}

	if (observe_value == COAP_OBSERVE_REGISTER) {
		ret = register_observer(resource, msg, msg_info);
		if (ret == 0 && observe_seq != NULL) {
			/* Return sequence number for use in response */
			struct coap_observer *obs = find_observer(resource, msg_info);
			if (obs) {
				*observe_seq = obs->observe_seq;
			}
		}
		return ret;
	} else if (observe_value == COAP_OBSERVE_DEREGISTER) {
		deregister_observer(resource, msg_info);
		return 1; /* Deregistered - return 1 to indicate no observe in response */
	}

	return -EINVAL;
}

int coap_observe_notify(struct coap_observe_resource *resource,
			const uint8_t *payload, int payload_len)
{
	otInstance *ot;
	int sent = 0;

	ot = openthread_get_default_instance();
	if (!ot) {
		return -ENODEV;
	}

	for (int i = 0; i < COAP_MAX_OBSERVERS; i++) {
		struct coap_observer *obs = &resource->observers[i];
		otMessage *msg;
		otError err;

		if (!obs->active) {
			continue;
		}

		msg = otCoapNewMessage(ot, NULL);
		if (!msg) {
			LOG_ERR("Failed to allocate notification message");
			continue;
		}

		/* Initialize as NON-confirmable notification */
		otCoapMessageInit(msg, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_CONTENT);

		/* Set same token as observer registration */
		err = otCoapMessageSetToken(msg, obs->token, obs->token_len);
		if (err != OT_ERROR_NONE) {
			otMessageFree(msg);
			continue;
		}

		/* Add Observe option with sequence number */
		obs->observe_seq++;
		err = otCoapMessageAppendObserveOption(msg, obs->observe_seq);
		if (err != OT_ERROR_NONE) {
			otMessageFree(msg);
			continue;
		}

		/* Add Content-Format: application/json */
		err = otCoapMessageAppendContentFormatOption(msg, OT_COAP_OPTION_CONTENT_FORMAT_JSON);
		if (err != OT_ERROR_NONE) {
			otMessageFree(msg);
			continue;
		}

		/* Add payload */
		err = otCoapMessageSetPayloadMarker(msg);
		if (err != OT_ERROR_NONE) {
			otMessageFree(msg);
			continue;
		}

		err = otMessageAppend(msg, payload, payload_len);
		if (err != OT_ERROR_NONE) {
			otMessageFree(msg);
			continue;
		}

		/* Send notification */
		err = otCoapSendRequest(ot, msg, &obs->peer_info, NULL, NULL);
		if (err != OT_ERROR_NONE) {
			LOG_WRN("Failed to send notification to observer: %s",
				otThreadErrorToString(err));
			/* Mark observer as inactive on send failure */
			obs->active = false;
			resource->observer_count--;
			otMessageFree(msg);
			continue;
		}

		obs->last_notify = k_uptime_get();
		sent++;

		LOG_DBG("Sent notification %u to observer for %s",
			obs->observe_seq, resource->uri_path);
	}

	if (sent > 0) {
		LOG_INF("Notified %d observers for %s", sent, resource->uri_path);
	}

	return sent;
}
