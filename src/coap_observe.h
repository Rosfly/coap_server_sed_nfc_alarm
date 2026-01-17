/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CoAP Observe (RFC 7641) support for push notifications
 */

#ifndef COAP_OBSERVE_H
#define COAP_OBSERVE_H

#include <zephyr/net/openthread.h>
#include <openthread/coap.h>

/* Maximum number of observers per resource */
#define COAP_MAX_OBSERVERS 4

/* Observe option values */
#define COAP_OBSERVE_REGISTER   0
#define COAP_OBSERVE_DEREGISTER 1

/* Observer entry */
struct coap_observer {
	bool active;
	otMessageInfo peer_info;
	uint8_t token[8];
	uint8_t token_len;
	uint32_t observe_seq;
	uint64_t last_notify;  /* Timestamp of last notification */
};

/* Resource with observe support */
struct coap_observe_resource {
	const char *uri_path;
	struct coap_observer observers[COAP_MAX_OBSERVERS];
	int observer_count;
};

/**
 * Check if incoming request has Observe option and handle registration
 *
 * @param resource The observe resource
 * @param msg Incoming CoAP message
 * @param msg_info Message info with peer address
 * @param observe_seq Output: sequence number to use in response (only if registered)
 * @return 0 if observe registered, 1 if deregistered, -ENOENT if no observe option
 */
int coap_observe_handle(struct coap_observe_resource *resource,
			otMessage *msg, const otMessageInfo *msg_info,
			uint32_t *observe_seq);

/**
 * Send notification to all observers of a resource
 *
 * @param resource The observe resource
 * @param payload Payload buffer
 * @param payload_len Payload length
 * @return Number of notifications sent
 */
int coap_observe_notify(struct coap_observe_resource *resource,
			const uint8_t *payload, int payload_len);

/**
 * Initialize observe resource
 *
 * @param resource The observe resource to initialize
 * @param uri_path URI path for this resource
 */
void coap_observe_init(struct coap_observe_resource *resource, const char *uri_path);

/**
 * Remove all observers (e.g., on network disconnect)
 *
 * @param resource The observe resource
 */
void coap_observe_clear(struct coap_observe_resource *resource);

#endif /* COAP_OBSERVE_H */
