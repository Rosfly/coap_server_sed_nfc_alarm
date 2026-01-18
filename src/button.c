/*
 * Copyright (c) 2024 Alexandre Bailon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(coap);

#include "coap_utils.h"
#include "coap_observe.h"
#include "button.h"

struct btn_rsc_data {
	const struct gpio_dt_spec gpio;
	struct gpio_callback cb;
	int last_state;
};

struct btn_rsc_ctx {
	struct btn_rsc_data *btn;
	int count;
	struct coap_observe_resource observe;
	struct k_work notify_work;  /* Work item for deferred notification */
};

static const struct json_obj_descr json_btn_state_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_btn_state, btn_id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct json_btn_state, state, JSON_TOK_NUMBER),
};

static const struct json_obj_descr json_btn_get_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct json_btn_get, device_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct json_btn_get, btns, JSON_MAX_BTN, count,
				 json_btn_state_descr, ARRAY_SIZE(json_btn_state_descr)),
};

static K_SEM_DEFINE(btn_get_sem, 0, 1);

#ifdef CONFIG_OT_COAP_SAMPLE_SERVER

/* Forward declarations */
static struct btn_rsc_ctx btn_rsc_ctx;
static void btn_notify_work_handler(struct k_work *work);

static int btn_build_state_payload(struct btn_rsc_ctx *btn_ctx, uint8_t *buf, int buf_size)
{
	struct json_btn_get btn_data = {
		.device_id = coap_device_id(),
	};

	for (int i = 0; i < btn_ctx->count; i++) {
		btn_data.btns[i].btn_id = i;
		/* Use gpio_pin_get_dt which respects GPIO_ACTIVE_LOW/HIGH flags
		 * Returns 1 when button is pressed (active), 0 when not pressed
		 */
		btn_data.btns[i].state = gpio_pin_get_dt(&btn_ctx->btn[i].gpio);
	}
	btn_data.count = btn_ctx->count;

	json_obj_encode_buf(json_btn_get_descr, ARRAY_SIZE(json_btn_get_descr), &btn_data, buf,
			    buf_size);

	return strlen((char *)buf) + 1;
}

void btn_notify_observers(void)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	int len;

	if (btn_rsc_ctx.observe.observer_count == 0) {
		return;
	}

	len = btn_build_state_payload(&btn_rsc_ctx, buf, COAP_MAX_BUF_SIZE);
	coap_observe_notify(&btn_rsc_ctx.observe, buf, len);
}

/* Work handler for deferred notification (called from thread context) */
static void btn_notify_work_handler(struct k_work *work)
{
	LOG_INF("btn_notify_work_handler called, observers=%d", btn_rsc_ctx.observe.observer_count);
	btn_notify_observers();
}

static int btn_handler_get(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	struct btn_rsc_ctx *btn_ctx = ctx;
	uint32_t observe_seq = 0;
	int len;
	int ret;

	/* Handle Observe registration/deregistration */
	ret = coap_observe_handle(&btn_ctx->observe, msg, msg_info, &observe_seq);
	LOG_INF("btn observe_handle ret=%d, observers=%d", ret, btn_ctx->observe.observer_count);

	len = btn_build_state_payload(btn_ctx, buf, COAP_MAX_BUF_SIZE);

	/* If observe registered (ret == 0), include Observe option in response */
	if (ret == 0) {
		return coap_resp_send_observe(msg, msg_info, buf, len, observe_seq);
	}

	return coap_resp_send(msg, msg_info, buf, len);
}

static void btn_handler(void *ctx, otMessage *msg, const otMessageInfo *msg_info)
{
	coap_req_handler(ctx, msg, msg_info, NULL, btn_handler_get);
}

#define DEFINE_BTN_CTX(node_id)                                                                    \
	{                                                                                          \
		.gpio = GPIO_DT_SPEC_GET(node_id, gpios),                                          \
	},

#define DEFINE_BTNS_CTX(inst, compat, ...) DT_FOREACH_CHILD(DT_INST(inst, compat), DEFINE_BTN_CTX)

static struct btn_rsc_data btn_rsc_data[] = {
	DT_COMPAT_FOREACH_STATUS_OKAY_VARGS(gpio_keys, DEFINE_BTNS_CTX)};

static struct btn_rsc_ctx btn_rsc_ctx = {
	.btn = btn_rsc_data,
	.count = ARRAY_SIZE(btn_rsc_data),
};

static otCoapResource btn_rsc = {
	.mUriPath = BTN_URI,
	.mHandler = btn_handler,
	.mContext = &btn_rsc_ctx,
	.mNext = NULL,
};

/* GPIO interrupt callback - called when button state changes (ISR context) */
static void btn_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	LOG_INF("Button GPIO interrupt fired, pins=0x%x", pins);
	/* Submit work to system workqueue - cannot call CoAP functions from ISR */
	int ret = k_work_submit(&btn_rsc_ctx.notify_work);
	LOG_INF("k_work_submit returned %d", ret);
}

static int button_init_rsc(otCoapResource *rsc)
{
	int ret = 0;

	struct btn_rsc_ctx *btn_ctx = rsc->mContext;

	LOG_INF("Initializing the buttons with observe support");
	for (int i = 0; i < btn_ctx->count; i++) {
		struct btn_rsc_data *btn = &btn_ctx->btn[i];
		const struct gpio_dt_spec *gpio = &btn->gpio;

		if (!gpio_is_ready_dt(gpio)) {
			LOG_ERR("Error: button device %s is not ready", gpio->port->name);
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(gpio, GPIO_INPUT);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure %s pin %d", ret,
				gpio->port->name, gpio->pin);
			return ret;
		}

		/* Configure interrupt for both edges to detect press and release */
		ret = gpio_pin_interrupt_configure_dt(gpio, GPIO_INT_EDGE_BOTH);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure interrupt on %s pin %d", ret,
				gpio->port->name, gpio->pin);
			return ret;
		}

		/* Setup GPIO callback for this button */
		gpio_init_callback(&btn->cb, btn_gpio_callback, BIT(gpio->pin));
		ret = gpio_add_callback(gpio->port, &btn->cb);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to add callback for %s pin %d", ret,
				gpio->port->name, gpio->pin);
			return ret;
		}

		btn->last_state = gpio_pin_get_dt(gpio);
		LOG_INF("Button %d initialized (pin %d)", i, gpio->pin);
	}

	return ret;
}

void coap_btn_reg_rsc(void)
{
	otInstance *ot = openthread_get_default_instance();

	button_init_rsc(&btn_rsc);
	coap_observe_init(&btn_rsc_ctx.observe, BTN_URI);

	/* Initialize work item for deferred notification from ISR */
	k_work_init(&btn_rsc_ctx.notify_work, btn_notify_work_handler);

	LOG_INF("Registering button rsc with observe support");
	otCoapAddResource(ot, &btn_rsc);
}
#endif /* CONFIG_OT_COAP_SAMPLE_SERVER */

int button_init(const struct gpio_dt_spec *gpio)
{
	int ret;

	if (!gpio_is_ready_dt(gpio)) {
		LOG_ERR("Error: button device %s is not ready\n", gpio->port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, gpio->port->name,
			gpio->pin);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n", ret,
			gpio->port->name, gpio->pin);
		return ret;
	}

	return 0;
}

static void coap_btn_get_state_cb(void *ctx, otMessage *msg, const otMessageInfo *msg_info,
				  otError error)
{
	uint8_t buf[COAP_MAX_BUF_SIZE];
	int len = COAP_MAX_BUF_SIZE;
	struct json_btn_get *btn = (struct json_btn_get *)ctx;
	int ret;

	if (error != OT_ERROR_NONE) {
		LOG_WRN("Button GET request failed: error=%d", error);
		btn->count = 0;
		goto exit;
	}

	if (!msg) {
		LOG_WRN("Button GET: no response message");
		btn->count = 0;
		goto exit;
	}

	ret = coap_get_data(msg, buf, &len);
	if (ret) {
		LOG_ERR("Failed to extract button state data: %d", ret);
		btn->count = 0;
		goto exit;
	}

	ret = json_obj_parse(buf, len, json_btn_get_descr, ARRAY_SIZE(json_btn_get_descr), btn);
	if (ret < 0) {
		LOG_ERR("Failed to parse button state JSON: %d", ret);
		btn->count = 0;
	}

exit:
	k_sem_give(&btn_get_sem);
}

int coap_btn_get_state(const char *addr, int btn_id, int *state)
{
	struct json_btn_get btn;
	int ret;

	ret = coap_get_req_send(addr, BTN_URI, NULL, 0, coap_btn_get_state_cb, &btn);
	if (ret) {
		return ret;
	}

	/* Wait up to 30 seconds for CoAP response */
	ret = k_sem_take(&btn_get_sem, K_SECONDS(10));
	if (ret == -EAGAIN) {
		LOG_WRN("Timeout waiting for button state response");
		return -ETIMEDOUT;
	} else if (ret) {
		return ret;
	}

	if (btn_id >= btn.count) {
		return -ENODEV;
	}

	*state = btn.btns[btn_id].state;
	return ret;
}
