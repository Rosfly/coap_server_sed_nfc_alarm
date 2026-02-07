/*
 * Copyright (c) 2024-2025
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(coap);

#include "coap_utils.h"
#include "coap_observe.h"
#include "led.h"

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER

/* Alarm output pin (P1.06) - push-pull pulse on LED ON */
#if DT_NODE_EXISTS(DT_NODELABEL(alarm_pin))
static const struct gpio_dt_spec alarm_pin =
	GPIO_DT_SPEC_GET(DT_NODELABEL(alarm_pin), gpios);
static struct k_timer alarm_pulse_timer;
static bool alarm_pin_ready;

static void alarm_pulse_timer_handler(struct k_timer *timer)
{
	/* Return pin to high-impedance */
	gpio_pin_configure_dt(&alarm_pin, GPIO_INPUT);
}
#endif
struct led_rsc_data {
	const struct gpio_dt_spec gpio;
	int state;
};

struct led_rsc_ctx {
	struct led_rsc_data *led;
	int count;
	struct coap_observe_resource observe;
};
#endif

static const struct json_obj_descr json_led_state_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_led_state, led_id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct json_led_state, state, JSON_TOK_NUMBER),
};

static const struct json_obj_descr json_led_get_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_led_get, device_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct json_led_get, leds, JSON_MAX_LED, count,
				 json_led_state_descr, ARRAY_SIZE(json_led_state_descr)),
};

K_SEM_DEFINE(led_get_sem, 0, 1);

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER
static int led_init(otCoapResource *rsc)
{
	struct led_rsc_ctx *led_ctx = rsc->mContext;
	int ret;

	LOG_INF("Initializing the LED");
	for (int i = 0; i < led_ctx->count; i++) {
		struct led_rsc_data *led = &led_ctx->led[i];

		if (!gpio_is_ready_dt(&led->gpio)) {
			return -ENODEV;
		}

		/* Configure as output and set to OFF (0) initially */
		ret = gpio_pin_configure_dt(&led->gpio, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			LOG_ERR("Failed to configure the GPIO");
			return ret;
		}

		/* Initialize state to OFF - matches GPIO_OUTPUT_INACTIVE */
		led->state = 0;
		LOG_INF("LED %d initialized to OFF", i);
	}

	/* Initialize alarm output pin */
#if DT_NODE_EXISTS(DT_NODELABEL(alarm_pin))
	if (gpio_is_ready_dt(&alarm_pin)) {
		gpio_pin_configure_dt(&alarm_pin, GPIO_INPUT);  /* start high-Z */
		k_timer_init(&alarm_pulse_timer, alarm_pulse_timer_handler, NULL);
		alarm_pin_ready = true;
		LOG_INF("Alarm output pin initialized (high-Z)");
	}
#endif

	return 0;
}

/* Forward declaration */
static void led_notify_observers(struct led_rsc_ctx *led_ctx);

static int led_handler_put(void *ctx, uint8_t *buf, int size)
{
	struct json_led_state led_data;
	struct led_rsc_ctx *led_ctx = ctx;
	struct led_rsc_data *led;
	int ret = -EINVAL;

	json_obj_parse(buf, size, json_led_state_descr, ARRAY_SIZE(json_led_state_descr),
		       &led_data);

	if (led_data.led_id >= led_ctx->count) {
		LOG_ERR("Invalid led id: %x", led_data.led_id);
		return -EINVAL;
	}
	led = &led_ctx->led[led_data.led_id];

	switch (led_data.state) {
	case LED_MSG_STATE_ON:
		ret = gpio_pin_set_dt(&led->gpio, 1);
		led->state = 1;
		break;
	case LED_MSG_STATE_OFF:
		ret = gpio_pin_set_dt(&led->gpio, 0);
		led->state = 0;
		break;
	case LED_MSG_STATE_TOGGLE:
		led->state = 1 - led->state;
		ret = gpio_pin_set_dt(&led->gpio, led->state);
		break;
	default:
		LOG_ERR("Set an unsupported LED state: %x", led_data.state);
	}

	/* Notify observers of state change */
	if (ret == 0) {
		led_notify_observers(led_ctx);
	}

	/* Trigger alarm output pulse on P1.06 for led_id 0 */
#if DT_NODE_EXISTS(DT_NODELABEL(alarm_pin))
	if (alarm_pin_ready && ret == 0 && led_data.led_id == 0) {
		if (led->state == 1) {
			/* Drive pin high (standard push-pull) for ~1 second */
			gpio_pin_configure_dt(&alarm_pin, GPIO_OUTPUT_HIGH);
			k_timer_start(&alarm_pulse_timer, K_SECONDS(1), K_NO_WAIT);
		} else {
			/* LED off: ensure high-Z, cancel any pending pulse */
			k_timer_stop(&alarm_pulse_timer);
			gpio_pin_configure_dt(&alarm_pin, GPIO_INPUT);
		}
	}
#endif

	return ret;
}

static int led_build_state_payload(struct led_rsc_ctx *led_ctx, uint8_t *buf, int buf_size)
{
	struct json_led_get led_data = {
		.device_id = coap_device_id(),
	};

	for (int i = 0; i < led_ctx->count; i++) {
		led_data.leds[i].led_id = i;
		led_data.leds[i].state = led_ctx->led[i].state;
	}
	led_data.count = led_ctx->count;

	json_obj_encode_buf(json_led_get_descr, ARRAY_SIZE(json_led_get_descr), &led_data, buf,
			    buf_size);

	return strlen((char *)buf) + 1;
}

static void led_notify_observers(struct led_rsc_ctx *led_ctx)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	int len;

	if (led_ctx->observe.observer_count == 0) {
		return;
	}

	len = led_build_state_payload(led_ctx, buf, COAP_MAX_BUF_SIZE);
	coap_observe_notify(&led_ctx->observe, buf, len);
}

static int led_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	struct led_rsc_ctx *led_ctx = ctx;
	uint32_t observe_seq = 0;
	int len;
	int ret;

	/* Handle Observe registration/deregistration */
	ret = coap_observe_handle(&led_ctx->observe, msg, msg_info, &observe_seq);
	LOG_INF("led observe_handle ret=%d, observers=%d", ret, led_ctx->observe.observer_count);

	len = led_build_state_payload(led_ctx, buf, COAP_MAX_BUF_SIZE);

	/* If observe registered (ret == 0), include Observe option in response */
	if (ret == 0) {
		return coap_resp_send_observe(msg, msg_info, buf, len, observe_seq);
	}

	return coap_resp_send(msg, msg_info, buf, len);
}

static void led_handler(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	coap_req_handler(ctx, msg, msg_info, led_handler_put, led_handler_get);
}

#define DEFINE_LED_CTX(node_id)                                                                    \
	{                                                                                          \
		.gpio = GPIO_DT_SPEC_GET(node_id, gpios),                                          \
		.state = 0,                                                                        \
	},

#define DEFINE_LEDS_CTX(inst, compat, ...) DT_FOREACH_CHILD(DT_INST(inst, compat), DEFINE_LED_CTX)

static struct led_rsc_data led_rsc_data[] = {
	DT_COMPAT_FOREACH_STATUS_OKAY_VARGS(gpio_leds, DEFINE_LEDS_CTX)};

static struct led_rsc_ctx led_rsc_ctx = {
	.led = led_rsc_data,
	.count = ARRAY_SIZE(led_rsc_data),
};

static otCoapResource led_rsc = {
	.mUriPath = LED_URI,
	.mHandler = led_handler,
	.mContext = &led_rsc_ctx,
	.mNext = NULL,
};

void coap_led_reg_rsc(void)
{
	otInstance *ot = openthread_get_default_instance();

	LOG_INF("Registering LED rsc");
	led_init(&led_rsc);
	coap_observe_init(&led_rsc_ctx.observe, LED_URI);
	otCoapAddResource(ot, &led_rsc);
}
#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */

static void coap_led_send_req_cb(void *ctx, otMessage *msg, const otMessageInfo *msg_info,
				 otError error)
{
	if (error != OT_ERROR_NONE) {
		LOG_WRN("LED CoAP request failed: error=%d", error);

		/* Common errors for mobile devices:
		 * OT_ERROR_NO_ROUTE: Device detached from network
		 * OT_ERROR_RESPONSE_TIMEOUT: Bridge/server not responding
		 * OT_ERROR_ABORT: Request was aborted
		 */
		if (error == OT_ERROR_NO_ROUTE) {
			LOG_INF("Network connection lost - will attempt reconnection");
		}
		return;
	}

	if (!msg) {
		LOG_WRN("LED CoAP request: no response message");
		return;
	}

	/* Response received successfully */
	LOG_DBG("LED command acknowledged");
}

int coap_led_set_state(const char *addr, int led_id, int state)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];

	struct json_led_state led_data = {
		.led_id = led_id,
		.state = state,
	};

	json_obj_encode_buf(json_led_state_descr, ARRAY_SIZE(json_led_state_descr), &led_data, buf,
			    COAP_MAX_BUF_SIZE);

	return coap_put_req_send(addr, LED_URI, buf, strlen(buf) + 1, coap_led_send_req_cb, NULL);
}

static void coap_led_get_state_cb(void *ctx, otMessage *msg, const otMessageInfo *msg_info,
				  otError error)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	int len = COAP_MAX_BUF_SIZE;
	struct json_led_get *led = (struct json_led_get *)ctx;
	int ret;

	if (error != OT_ERROR_NONE) {
		LOG_WRN("LED GET request failed: error=%d", error);
		led->count = 0;
		goto exit;
	}

	if (!msg) {
		LOG_WRN("LED GET: no response message");
		led->count = 0;
		goto exit;
	}

	ret = coap_get_data(msg, buf, &len);
	if (ret) {
		LOG_ERR("Failed to extract LED state data: %d", ret);
		led->count = 0;
		goto exit;
	}

	ret = json_obj_parse(buf, len, json_led_get_descr, ARRAY_SIZE(json_led_get_descr), led);
	if (ret < 0) {
		LOG_ERR("Failed to parse LED state JSON: %d", ret);
		led->count = 0;
	}

exit:
	k_sem_give(&led_get_sem);
}

int coap_led_get_state(const char *addr, int led_id, int *state)
{
	struct json_led_get led;
	int ret;

	ret = coap_get_req_send(addr, LED_URI, NULL, 0, coap_led_get_state_cb, &led);
	if (ret) {
		return ret;
	}

	/* Wait up to 10 seconds for CoAP response (CoAP default ~4-8s with retries) */
	ret = k_sem_take(&led_get_sem, K_SECONDS(10));
	if (ret == -EAGAIN) {
		LOG_WRN("Timeout waiting for LED state response");
		return -ETIMEDOUT;
	} else if (ret) {
		return ret;
	}

	if (led_id >= led.count) {
		return -ENODEV;
	}

	*state = led.leds[led_id].state;
	return ret;
}
