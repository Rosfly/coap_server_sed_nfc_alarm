/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NETWORK_MONITOR_H
#define NETWORK_MONITOR_H

/**
 * @brief Start network state monitoring thread
 *
 * Monitors Thread network state and automatically triggers
 * reconnection when device becomes detached.
 *
 * @return 0 on success, negative error code otherwise
 */
int network_monitor_start(void);

/**
 * @brief Stop network monitoring thread
 */
void network_monitor_stop(void);

#endif /* NETWORK_MONITOR_H */
