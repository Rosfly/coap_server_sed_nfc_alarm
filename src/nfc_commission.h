/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NFC_COMMISSION_H
#define NFC_COMMISSION_H

#include <stdbool.h>

/**
 * @brief NFC commissioning states
 */
enum nfc_commission_state {
	NFC_STATE_IDLE,           /* Not initialized */
	NFC_STATE_WAITING,        /* Waiting for NFC dataset */
	NFC_STATE_PROCESSING,     /* Processing received NDEF */
	NFC_STATE_COMMISSIONED,   /* Dataset applied successfully */
	NFC_STATE_ERROR,          /* Error occurred */
};

/**
 * @brief Initialize NFC commissioning subsystem
 *
 * Sets up NFC Type 4 Tag in read-write mode.
 *
 * @return 0 on success, negative error code on failure
 */
int nfc_commission_init(void);

/**
 * @brief Start NFC commissioning and block until dataset received
 *
 * Enables NFC emulation and waits for a phone to write an NDEF message
 * containing the Thread Active Dataset as a hex-encoded text record.
 * LED blinks to indicate waiting state.
 *
 * @return 0 on success (commissioned), negative error code on failure
 */
int nfc_commission_start(void);

/**
 * @brief Get current commissioning state
 *
 * @return Current NFC commissioning state
 */
enum nfc_commission_state nfc_commission_get_state(void);

/**
 * @brief Stop NFC and release resources
 *
 * Call after commissioning complete to save power.
 */
void nfc_commission_stop(void);

#endif /* NFC_COMMISSION_H */
