/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network State Monitoring for Thread CoAP Client
 * Monitors Thread network state and triggers reconnection
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>

LOG_MODULE_REGISTER(network_monitor, LOG_LEVEL_INF);

#define MONITOR_INTERVAL K_SECONDS(5)
#define RECONNECT_DELAY K_SECONDS(10)
#define MAX_DETACHED_COUNT 6  /* Max detached checks before forcing restart (6 * 10s = 1 min) */

static K_THREAD_STACK_DEFINE(monitor_stack, 2048);
static struct k_thread monitor_thread_data;
static bool monitor_running = false;

/* Last known network state */
static otDeviceRole last_role = OT_DEVICE_ROLE_DISABLED;
static int detached_count = 0;

/**
 * Force restart the Thread stack.
 * This is needed when the device has been detached for too long and
 * automatic reattachment isn't working.
 */
static void thread_force_restart(otInstance *ot)
{
	otError err;

	LOG_WRN("Force restarting Thread stack...");

	/* Step 1: Stop Thread */
	err = otThreadSetEnabled(ot, false);
	if (err != OT_ERROR_NONE) {
		LOG_ERR("Failed to disable Thread: %d", err);
	}

	k_sleep(K_MSEC(500));

	/* Step 2: Disable IPv6 interface */
	err = otIp6SetEnabled(ot, false);
	if (err != OT_ERROR_NONE) {
		LOG_ERR("Failed to disable IPv6: %d", err);
	}

	k_sleep(K_MSEC(500));

	/* Step 3: Re-enable IPv6 interface */
	err = otIp6SetEnabled(ot, true);
	if (err != OT_ERROR_NONE) {
		LOG_ERR("Failed to enable IPv6: %d", err);
	}

	k_sleep(K_MSEC(100));

	/* Step 4: Start Thread again */
	err = otThreadSetEnabled(ot, true);
	if (err != OT_ERROR_NONE) {
		LOG_ERR("Failed to enable Thread: %d", err);
	}

	LOG_INF("Thread stack restarted - waiting for attachment...");
}

static void network_state_changed_callback(otChangedFlags flags, void *context)
{
	ARG_UNUSED(context);

	if (flags & OT_CHANGED_THREAD_ROLE) {
		otInstance *ot = openthread_get_default_instance();
		otDeviceRole role = otThreadGetDeviceRole(ot);

		LOG_INF("Thread role changed: %d -> %d", last_role, role);

		if (role == OT_DEVICE_ROLE_DETACHED || role == OT_DEVICE_ROLE_DISABLED) {
			LOG_WRN("Device lost network connection");
		} else if (role == OT_DEVICE_ROLE_CHILD) {
			LOG_INF("Device attached to network as child (MTD)");
			detached_count = 0;  /* Reset counter on successful attach */
		} else if (role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
			LOG_INF("Device attached to network as router/leader (FTD)");
			detached_count = 0;  /* Reset counter on successful attach */
		}

		last_role = role;
	}

	if (flags & OT_CHANGED_THREAD_PARTITION_ID) {
		LOG_INF("Network partition changed - device may have moved");
	}
}

static void monitor_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	otInstance *ot = openthread_get_default_instance();

	if (!ot) {
		LOG_ERR("Failed to get OpenThread instance");
		return;
	}

	LOG_INF("Network monitor thread started");

	/* Register state change callback */
	otError error = otSetStateChangedCallback(ot, network_state_changed_callback, NULL);
	if (error != OT_ERROR_NONE) {
		LOG_ERR("Failed to register state callback: %d", error);
	}

	/* Get initial state */
	last_role = otThreadGetDeviceRole(ot);
	LOG_INF("Initial Thread role: %d", last_role);

	while (monitor_running) {
		otDeviceRole role = otThreadGetDeviceRole(ot);
		bool ip6_enabled = otIp6IsEnabled(ot);

		/* Check if IPv6 interface is down - this can happen after extended disconnection */
		if (!ip6_enabled) {
			LOG_ERR("IPv6 interface is disabled - re-enabling");
			otError err = otIp6SetEnabled(ot, true);
			if (err != OT_ERROR_NONE) {
				LOG_ERR("Failed to enable IPv6: %d", err);
			}
			k_sleep(K_MSEC(500));
			continue;
		}

		/* Check if we're detached and should try to reattach */
		if (role == OT_DEVICE_ROLE_DETACHED) {
			detached_count++;
			LOG_WRN("Device detached (%d/%d) - waiting for reattach",
				detached_count, MAX_DETACHED_COUNT);

			/* If we've been detached too long, force restart the stack */
			if (detached_count >= MAX_DETACHED_COUNT) {
				LOG_WRN("Detached for too long - forcing Thread restart");
				thread_force_restart(ot);
				detached_count = 0;
			}

			/* Wait longer before next check when detached */
			k_sleep(RECONNECT_DELAY);
		} else if (role == OT_DEVICE_ROLE_DISABLED) {
			LOG_ERR("Thread is disabled - re-enabling");
			otError err = otThreadSetEnabled(ot, true);
			if (err != OT_ERROR_NONE) {
				LOG_ERR("Failed to enable Thread: %d", err);
			}
			detached_count = 0;
			k_sleep(RECONNECT_DELAY);
		} else {
			/* Connected (child/router/leader) - reset counter */
			if (detached_count > 0) {
				LOG_INF("Reconnected after %d detached cycles", detached_count);
				detached_count = 0;
			}
			/* Everything OK, check less frequently */
			k_sleep(MONITOR_INTERVAL);
		}
	}

	LOG_INF("Network monitor thread stopped");
}

int network_monitor_start(void)
{
	if (monitor_running) {
		LOG_WRN("Network monitor already running");
		return -EALREADY;
	}

	monitor_running = true;

	k_thread_create(&monitor_thread_data, monitor_stack,
			K_THREAD_STACK_SIZEOF(monitor_stack),
			monitor_thread_entry,
			NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);

	k_thread_name_set(&monitor_thread_data, "net_monitor");

	LOG_INF("Network monitor started");
	return 0;
}

void network_monitor_stop(void)
{
	monitor_running = false;
	LOG_INF("Network monitor stop requested");
}
