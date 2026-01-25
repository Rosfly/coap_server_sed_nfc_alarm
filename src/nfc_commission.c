/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nfc_commission, CONFIG_LOG_DEFAULT_LEVEL);

#include <nfc_t4t_lib.h>
#include <nfc/ndef/msg_parser.h>
#include <nfc/t4t/ndef_file.h>

#include <openthread/dataset.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>
#include <zephyr/net/openthread.h>

#include "nfc_commission.h"

/* NDEF file buffer - must be large enough for Thread dataset (max 254 bytes = 508 hex chars) */
#define NDEF_FILE_SIZE 512
static uint8_t ndef_msg_buf[NDEF_FILE_SIZE];

/* Parsed dataset TLVs - stored by work handler, applied by main thread */
static otOperationalDatasetTlvs parsed_dataset;
static bool dataset_ready = false;

/* Current state */
static enum nfc_commission_state current_state = NFC_STATE_IDLE;

/* Semaphore to signal when NDEF is updated */
static K_SEM_DEFINE(ndef_updated_sem, 0, 1);

/* LED for status indication */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* LED blink timer */
static struct k_timer blink_timer;
static bool led_state;

/* Work item for processing NDEF in thread context */
static struct k_work process_ndef_work;

/**
 * @brief Convert a hex character to its numeric value
 */
static int hex_char_to_val(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	} else if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	} else if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/**
 * @brief Convert hex string to binary data
 *
 * @param hex_str Input hex string
 * @param hex_len Length of hex string
 * @param bin_buf Output binary buffer
 * @param bin_max Maximum size of binary buffer
 * @return Number of bytes written, or negative error
 */
static int hex_to_binary(const uint8_t *hex_str, size_t hex_len,
			 uint8_t *bin_buf, size_t bin_max)
{
	size_t i;
	size_t bin_idx = 0;

	/* Skip leading whitespace and any text prefix */
	while (hex_len > 0 && (*hex_str == ' ' || *hex_str == '\t' ||
			       *hex_str == '\n' || *hex_str == '\r')) {
		hex_str++;
		hex_len--;
	}

	/* Hex string must have even length */
	if (hex_len % 2 != 0) {
		LOG_ERR("Hex string has odd length: %zu", hex_len);
		return -EINVAL;
	}

	if (hex_len / 2 > bin_max) {
		LOG_ERR("Binary output too large: %zu > %zu", hex_len / 2, bin_max);
		return -ENOMEM;
	}

	for (i = 0; i < hex_len; i += 2) {
		int high = hex_char_to_val(hex_str[i]);
		int low = hex_char_to_val(hex_str[i + 1]);

		if (high < 0 || low < 0) {
			LOG_ERR("Invalid hex char at position %zu", i);
			return -EINVAL;
		}

		bin_buf[bin_idx++] = (high << 4) | low;
	}

	return bin_idx;
}

/**
 * @brief Parse hex string and store in static buffer (called from work queue)
 *
 * This only parses the data - OpenThread APIs are called later from main thread
 */
static int parse_dataset_from_hex(const uint8_t *hex_str, size_t hex_len)
{
	int bin_len;

	/* Convert hex string to binary TLVs */
	bin_len = hex_to_binary(hex_str, hex_len, parsed_dataset.mTlvs,
				sizeof(parsed_dataset.mTlvs));
	if (bin_len < 0) {
		LOG_ERR("Failed to convert hex to binary: %d", bin_len);
		return bin_len;
	}
	parsed_dataset.mLength = bin_len;
	dataset_ready = true;

	LOG_INF("Dataset parsed (%d bytes) - ready to apply", bin_len);
	return 0;
}

/**
 * @brief Apply stored dataset and start Thread (called from main thread)
 *
 * OpenThread APIs must be called from main thread context, not work queue
 */
static int apply_stored_dataset(void)
{
	otInstance *ot;
	otError error;

	if (!dataset_ready) {
		LOG_ERR("No dataset ready to apply");
		return -ENODATA;
	}

	/* Get OpenThread instance (new API - direct access) */
	ot = openthread_get_default_instance();
	if (!ot) {
		LOG_ERR("Failed to get OpenThread instance");
		return -ENODEV;
	}

	LOG_INF("Applying Thread dataset (%d bytes)", parsed_dataset.mLength);

	/* Lock OpenThread API (new API - no context parameter) */
	openthread_mutex_lock();

	/* Apply the dataset - equivalent to "ot dataset set active xxx" */
	error = otDatasetSetActiveTlvs(ot, &parsed_dataset);
	if (error != OT_ERROR_NONE) {
		openthread_mutex_unlock();
		LOG_ERR("Failed to set dataset: %d", error);
		return -EIO;
	}

	LOG_INF("Dataset applied - starting Thread stack");

	/* Enable IPv6 interface - equivalent to "ot ifconfig up" */
	error = otIp6SetEnabled(ot, true);
	if (error != OT_ERROR_NONE) {
		openthread_mutex_unlock();
		LOG_ERR("Failed to enable IPv6: %d", error);
		return -EIO;
	}

	/* Start Thread - equivalent to "ot thread start" */
	error = otThreadSetEnabled(ot, true);
	if (error != OT_ERROR_NONE) {
		openthread_mutex_unlock();
		LOG_ERR("Failed to start Thread: %d", error);
		return -EIO;
	}

	openthread_mutex_unlock();
	dataset_ready = false;

	LOG_INF("Thread started - device will join network");
	return 0;
}

/**
 * @brief Extract payload from NDEF Text record
 *
 * Text record format: [status byte][language code][text]
 * Status byte: bit 7 = UTF-16 flag, bits 5-0 = language code length
 */
static int extract_text_payload(const uint8_t *payload, size_t payload_len,
				const uint8_t **text, size_t *text_len)
{
	uint8_t status;
	uint8_t lang_len;

	if (payload_len < 1) {
		return -EINVAL;
	}

	status = payload[0];
	lang_len = status & 0x3F;  /* Lower 6 bits = language code length */

	if (payload_len < 1 + lang_len) {
		return -EINVAL;
	}

	*text = payload + 1 + lang_len;
	*text_len = payload_len - 1 - lang_len;

	return 0;
}

/**
 * @brief Process received NDEF message
 */
static int process_ndef_message(void)
{
	uint8_t result_buf[NFC_NDEF_PARSER_REQUIRED_MEM(2)];
	uint32_t result_len = sizeof(result_buf);
	uint32_t raw_len;
	const uint8_t *ndef_data;
	const struct nfc_ndef_msg_desc *msg;
	const struct nfc_ndef_record_desc *record;
	const struct nfc_ndef_bin_payload_desc *bin_pay;
	const uint8_t *text_payload;
	size_t text_len;
	int err;

	/* Get NDEF message length from NLEN field (first 2 bytes, big endian) */
	raw_len = (ndef_msg_buf[0] << 8) | ndef_msg_buf[1];
	if (raw_len == 0 || raw_len > NDEF_FILE_SIZE - NFC_NDEF_FILE_NLEN_FIELD_SIZE) {
		LOG_ERR("Invalid NDEF message length: %u", raw_len);
		return -EINVAL;
	}

	/* Skip NLEN field to get to actual NDEF message */
	ndef_data = nfc_t4t_ndef_file_msg_get(ndef_msg_buf);

	LOG_INF("Processing NDEF message (%u bytes)", raw_len);

	/* Parse NDEF message */
	err = nfc_ndef_msg_parse(result_buf, &result_len, ndef_data, &raw_len);
	if (err) {
		LOG_ERR("NDEF parse failed: %d", err);
		return err;
	}

	msg = (const struct nfc_ndef_msg_desc *)result_buf;

	if (msg->record_count < 1) {
		LOG_ERR("No NDEF records found");
		return -ENODATA;
	}

	LOG_INF("Found %d NDEF record(s)", msg->record_count);

	/* Get first record */
	record = msg->record[0];

	/* Check if it's a Text record (TNF=Well-Known, Type="T") */
	if (record->tnf != TNF_WELL_KNOWN) {
		LOG_ERR("Expected Well-Known TNF, got %d", record->tnf);
		return -EINVAL;
	}

	if (record->type_length != 1 || record->type[0] != 'T') {
		LOG_ERR("Expected Text record type 'T'");
		return -EINVAL;
	}

	/* Get binary payload descriptor */
	bin_pay = (const struct nfc_ndef_bin_payload_desc *)record->payload_descriptor;
	if (!bin_pay || !bin_pay->payload || bin_pay->payload_length == 0) {
		LOG_ERR("Empty payload");
		return -ENODATA;
	}

	/* Extract text from Text record (skip status byte and language code) */
	err = extract_text_payload(bin_pay->payload, bin_pay->payload_length,
				   &text_payload, &text_len);
	if (err) {
		LOG_ERR("Failed to extract text payload: %d", err);
		return err;
	}

	LOG_INF("Text payload: %zu bytes", text_len);

	/* Parse the hex-encoded dataset (will be applied from main thread) */
	return parse_dataset_from_hex(text_payload, text_len);
}

/**
 * @brief Work handler for processing NDEF in thread context
 */
static void process_ndef_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	LOG_INF("Processing NDEF message...");

	err = process_ndef_message();
	if (err) {
		LOG_ERR("Failed to process NDEF: %d", err);
		current_state = NFC_STATE_ERROR;
	} else {
		current_state = NFC_STATE_COMMISSIONED;
	}

	/* Signal completion */
	k_sem_give(&ndef_updated_sem);
}

/**
 * @brief NFC callback handler
 */
static void nfc_callback(void *context, nfc_t4t_event_t event,
			 const uint8_t *data, size_t data_length,
			 uint32_t flags)
{
	ARG_UNUSED(context);
	ARG_UNUSED(data);
	ARG_UNUSED(flags);

	switch (event) {
	case NFC_T4T_EVENT_FIELD_ON:
		LOG_INF("NFC field detected");
		/* Rapid blink while phone is near */
		k_timer_start(&blink_timer, K_MSEC(100), K_MSEC(100));
		break;

	case NFC_T4T_EVENT_FIELD_OFF:
		LOG_INF("NFC field removed");
		/* Return to slow blink if still waiting */
		if (current_state == NFC_STATE_WAITING) {
			k_timer_start(&blink_timer, K_MSEC(500), K_MSEC(500));
		}
		break;

	case NFC_T4T_EVENT_NDEF_READ:
		LOG_INF("NDEF message read by phone");
		break;

	case NFC_T4T_EVENT_NDEF_UPDATED:
		if (data_length > 0) {
			LOG_INF("NDEF message updated (%zu bytes)", data_length);
			current_state = NFC_STATE_PROCESSING;
			/* Process in work queue (not interrupt context) */
			k_work_submit(&process_ndef_work);
		}
		break;

	default:
		break;
	}
}

/**
 * @brief LED blink timer handler
 */
static void blink_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	led_state = !led_state;
	gpio_pin_set_dt(&led, led_state ? 1 : 0);
}

int nfc_commission_init(void)
{
	int err;

	LOG_INF("Initializing NFC commissioning");

	/* Initialize LED */
	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure LED GPIO: %d", err);
		return err;
	}

	/* Initialize blink timer */
	k_timer_init(&blink_timer, blink_timer_handler, NULL);

	/* Initialize work item */
	k_work_init(&process_ndef_work, process_ndef_work_handler);

	/* Setup NFC T4T */
	err = nfc_t4t_setup(nfc_callback, NULL);
	if (err) {
		LOG_ERR("NFC T4T setup failed: %d", err);
		return err;
	}

	/* Set up Read-Write mode with our buffer */
	err = nfc_t4t_ndef_rwpayload_set(ndef_msg_buf, sizeof(ndef_msg_buf));
	if (err) {
		LOG_ERR("NFC payload setup failed: %d", err);
		return err;
	}

	current_state = NFC_STATE_IDLE;

	LOG_INF("NFC commissioning initialized");
	return 0;
}

int nfc_commission_start(void)
{
	int err;

	LOG_INF("Starting NFC commissioning - waiting for dataset");

	/* Start NFC emulation */
	err = nfc_t4t_emulation_start();
	if (err) {
		LOG_ERR("NFC emulation start failed: %d", err);
		return err;
	}

	current_state = NFC_STATE_WAITING;

	/* Start slow LED blink to indicate waiting */
	led_state = false;
	k_timer_start(&blink_timer, K_MSEC(500), K_MSEC(500));

	LOG_INF("NFC active - tap phone with Thread dataset to commission");

	/* Wait for dataset to be received and processed */
	while (current_state == NFC_STATE_WAITING ||
	       current_state == NFC_STATE_PROCESSING) {
		k_sem_take(&ndef_updated_sem, K_MSEC(100));

		/* Handle error state - blink error pattern and retry */
		if (current_state == NFC_STATE_ERROR) {
			LOG_WRN("Invalid data received, waiting for retry...");

			/* Error blink pattern: 3 rapid blinks */
			for (int i = 0; i < 6; i++) {
				gpio_pin_set_dt(&led, i % 2);
				k_sleep(K_MSEC(100));
			}
			gpio_pin_set_dt(&led, 0);
			k_sleep(K_MSEC(500));

			/* Return to waiting state */
			current_state = NFC_STATE_WAITING;
			k_timer_start(&blink_timer, K_MSEC(500), K_MSEC(500));
		}
	}

	/* Stop blink timer */
	k_timer_stop(&blink_timer);

	if (current_state == NFC_STATE_COMMISSIONED) {
		/* Dataset parsed successfully - now apply it from main thread context */
		LOG_INF("NDEF parsed - applying dataset from main thread");

		err = apply_stored_dataset();
		if (err) {
			LOG_ERR("Failed to apply dataset: %d", err);
			/* Error blink */
			for (int i = 0; i < 6; i++) {
				gpio_pin_set_dt(&led, i % 2);
				k_sleep(K_MSEC(100));
			}
			gpio_pin_set_dt(&led, 0);
			return err;
		}

		/* Success - LED solid on briefly, then off */
		gpio_pin_set_dt(&led, 1);
		k_sleep(K_MSEC(1000));
		gpio_pin_set_dt(&led, 0);

		LOG_INF("NFC commissioning successful!");
		return 0;
	}

	return -EIO;
}

enum nfc_commission_state nfc_commission_get_state(void)
{
	return current_state;
}

void nfc_commission_stop(void)
{
	int err;

	LOG_INF("Stopping NFC commissioning");

	k_timer_stop(&blink_timer);
	gpio_pin_set_dt(&led, 0);

	err = nfc_t4t_emulation_stop();
	if (err) {
		LOG_WRN("NFC emulation stop failed: %d", err);
	}

	current_state = NFC_STATE_IDLE;
}
