/*
 * Copyright (c) 2024-2025
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(coap);

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>

#include "coap_utils.h"
#include "battery.h"

/*
 * ============================================================
 * ADC Configuration from Device Tree
 * ============================================================
 */
#define VBAT_NODE DT_PATH(vbat)

#if DT_NODE_EXISTS(VBAT_NODE)
/* ADC channel spec from voltage-divider node */
static const struct adc_dt_spec vbat_adc = ADC_DT_SPEC_GET(VBAT_NODE);

/* GPIO for TPS22916C load switch enable (saves power when not measuring) */
static const struct gpio_dt_spec vbat_enable = GPIO_DT_SPEC_GET(VBAT_NODE, power_gpios);

/* Voltage divider ratio from device tree: full_ohms / output_ohms */
#define VBAT_DIVIDER_FULL   DT_PROP(VBAT_NODE, full_ohms)
#define VBAT_DIVIDER_OUTPUT DT_PROP(VBAT_NODE, output_ohms)

/* ADC sample buffer and sequence */
static int16_t adc_sample_buffer;
static struct adc_sequence adc_sequence = {
	.buffer = &adc_sample_buffer,
	.buffer_size = sizeof(adc_sample_buffer),
};

static bool battery_adc_initialized = false;
#endif /* DT_NODE_EXISTS(VBAT_NODE) */

/* Last measured values */
static int32_t battery_voltage_mv = BATTERY_VOLTAGE_MV_DEFAULT;
static int32_t battery_percentage = BATTERY_PERCENTAGE_DEFAULT;

/* Timestamp of last measurement (for staleness check) */
static int64_t last_measurement_time_ms = 0;

/* Maximum age of cached voltage before triggering fresh measurement (120 seconds) */
#define VOLTAGE_STALE_THRESHOLD_MS (120 * 1000)

#if DT_NODE_EXISTS(VBAT_NODE)

/*
 * ============================================================
 * LiPo Battery Discharge Curve Lookup Table
 * ============================================================
 * Non-linear discharge curve for typical LiPo 3.7V cell
 * Based on actual discharge characteristics at ~0.2C rate
 */
struct voltage_percent_entry {
	int32_t voltage_mv;
	int32_t percentage;
};

static const struct voltage_percent_entry lipo_discharge_table[] = {
	{ 4200, 100 },  /* Fully charged */
	{ 4150,  95 },
	{ 4100,  90 },
	{ 4050,  85 },
	{ 4000,  80 },
	{ 3950,  75 },
	{ 3900,  70 },
	{ 3850,  65 },
	{ 3800,  60 },  /* Nominal voltage */
	{ 3750,  55 },
	{ 3700,  50 },
	{ 3650,  45 },
	{ 3600,  40 },
	{ 3550,  35 },
	{ 3500,  30 },
	{ 3450,  25 },
	{ 3400,  20 },
	{ 3350,  15 },
	{ 3300,  10 },
	{ 3200,   5 },
	{ 3000,   0 },  /* Cutoff voltage - do not discharge below */
};

#define LIPO_TABLE_SIZE ARRAY_SIZE(lipo_discharge_table)

/*
 * Calculate battery percentage from voltage using lookup table
 * with linear interpolation between points
 */
static int32_t voltage_to_percentage(int32_t voltage_mv)
{
	/* Handle edge cases */
	if (voltage_mv >= lipo_discharge_table[0].voltage_mv) {
		return 100;
	}
	if (voltage_mv <= lipo_discharge_table[LIPO_TABLE_SIZE - 1].voltage_mv) {
		return 0;
	}

	/* Find the two points to interpolate between */
	for (int i = 0; i < LIPO_TABLE_SIZE - 1; i++) {
		int32_t v_high = lipo_discharge_table[i].voltage_mv;
		int32_t v_low = lipo_discharge_table[i + 1].voltage_mv;

		if (voltage_mv <= v_high && voltage_mv > v_low) {
			/* Linear interpolation */
			int32_t p_high = lipo_discharge_table[i].percentage;
			int32_t p_low = lipo_discharge_table[i + 1].percentage;

			int32_t percentage = p_low +
				((voltage_mv - v_low) * (p_high - p_low)) / (v_high - v_low);
			return percentage;
		}
	}

	return 0;
}
/*
 * Initialize ADC and GPIO for battery measurement
 */
static int battery_adc_init(void)
{
	int ret;

	if (battery_adc_initialized) {
		return 0;
	}

	/* Check if ADC device is ready */
	if (!adc_is_ready_dt(&vbat_adc)) {
		LOG_ERR("ADC device not ready");
		return -ENODEV;
	}

	/* Setup ADC channel from device tree */
	ret = adc_channel_setup_dt(&vbat_adc);
	if (ret != 0) {
		LOG_ERR("ADC channel setup failed: %d", ret);
		return ret;
	}

	/* Initialize ADC sequence from device tree */
	ret = adc_sequence_init_dt(&vbat_adc, &adc_sequence);
	if (ret != 0) {
		LOG_ERR("ADC sequence init failed: %d", ret);
		return ret;
	}

	/* Check if GPIO device is ready */
	if (!device_is_ready(vbat_enable.port)) {
		LOG_ERR("VBAT enable GPIO not ready");
		return -ENODEV;
	}

	/* Configure GPIO as output, initially disabled (low) to save power */
	ret = gpio_pin_configure_dt(&vbat_enable, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("VBAT enable GPIO configure failed: %d", ret);
		return ret;
	}

	battery_adc_initialized = true;
	LOG_INF("Battery ADC initialized (divider: %d/%d = x%d.%d)",
		VBAT_DIVIDER_FULL, VBAT_DIVIDER_OUTPUT,
		VBAT_DIVIDER_FULL / VBAT_DIVIDER_OUTPUT,
		((VBAT_DIVIDER_FULL * 10) / VBAT_DIVIDER_OUTPUT) % 10);

	return 0;
}

/*
 * Perform battery voltage measurement
 * Enables TPS22916C, reads ADC, applies voltage divider correction, disables TPS22916C
 */
static int battery_measure(void)
{
	int ret;

	/* Initialize ADC if not already done */
	ret = battery_adc_init();
	if (ret != 0) {
		return ret;
	}

	/* Enable TPS22916C load switch to power voltage divider */
	ret = gpio_pin_set_dt(&vbat_enable, 1);
	if (ret != 0) {
		LOG_ERR("Failed to enable VBAT measurement: %d", ret);
		return ret;
	}

	/* Wait for TPS22916C turn-on and voltage divider to stabilize
	 * TPS22916C typical turn-on time: ~60µs
	 * Add margin for RC settling and ADC input capacitor charging
	 */
	k_sleep(K_MSEC(10));

	/* Perform ADC read */
	ret = adc_read_dt(&vbat_adc, &adc_sequence);

	/* Disable TPS22916C immediately after reading to save power */
	gpio_pin_set_dt(&vbat_enable, 0);

	if (ret != 0) {
		LOG_ERR("ADC read failed: %d", ret);
		return ret;
	}

	/* Convert raw ADC value to millivolts at ADC pin */
	int32_t adc_pin_mv = adc_sample_buffer;
	ret = adc_raw_to_millivolts_dt(&vbat_adc, &adc_pin_mv);
	if (ret != 0) {
		LOG_ERR("ADC conversion failed: %d", ret);
		return ret;
	}

	/* Apply voltage divider ratio: VBAT = Vadc * (full_ohms / output_ohms) */
	battery_voltage_mv = (adc_pin_mv * VBAT_DIVIDER_FULL) / VBAT_DIVIDER_OUTPUT;

	/* Calculate percentage from voltage using lookup table */
	battery_percentage = voltage_to_percentage(battery_voltage_mv);

	/* Update timestamp for staleness tracking */
	last_measurement_time_ms = k_uptime_get();

	LOG_INF("Battery: %d mV (%d%%), ADC pin: %d mV, raw: %d",
		battery_voltage_mv, battery_percentage, adc_pin_mv, adc_sample_buffer);

	return 0;
}
#else
/* Fallback when VBAT node doesn't exist in device tree */
static int battery_measure(void)
{
	LOG_WRN("Battery ADC not configured in device tree, using defaults");
	return 0;
}
#endif /* DT_NODE_EXISTS(VBAT_NODE) */

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
 * Performs fresh measurement and returns: {"device_id":"<eui64>","value":70}
 */
static int battery_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];

	/* Perform fresh battery measurement */
	battery_measure();

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
 * Uses cached voltage from last measurement: {"device_id":"<eui64>","value":3800}
 * Value is in millivolts (3800 = 3.800V)
 *
 * If cached value is stale (>120s old), triggers fresh measurement first.
 * This handles cases where voltage is queried before battery, or battery query failed.
 */
static int voltage_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	int64_t now = k_uptime_get();
	int64_t age_ms = now - last_measurement_time_ms;

	/* Refresh measurement if cached value is stale or never measured */
	if (last_measurement_time_ms == 0 || age_ms > VOLTAGE_STALE_THRESHOLD_MS) {
		LOG_INF("Voltage cache stale (%lld ms old), refreshing...", age_ms);
		battery_measure();
	}

	struct json_voltage_get voltage_data = {
		.device_id = coap_device_id(),
		.value = battery_voltage_mv,
	};

	json_obj_encode_buf(json_voltage_get_descr, ARRAY_SIZE(json_voltage_get_descr),
			    &voltage_data, buf, COAP_MAX_BUF_SIZE);

	LOG_INF("Voltage GET: %d.%03dV", battery_voltage_mv / 1000, battery_voltage_mv % 1000);

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
