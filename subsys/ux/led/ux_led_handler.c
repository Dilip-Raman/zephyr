/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zephyr/ux/ux.h>
#include <zephyr/ux/ux_led.h>

LOG_MODULE_REGISTER(ux_led, CONFIG_UX_LOG_LEVEL);

extern const struct ux_led_backend_api ux_led_backend;

static struct ux_led_caps backend_caps;

/* Led State Machine pattern step*/
struct ux_led_sm {
	const struct ux_led_pattern *pattern;
	size_t	step_idx;
	uint32_t	step_start_ms;
	struct ux_led_rgb	step_start_color;
	bool 	running;
};

static struct ux_led_sm led_sm;
static struct k_work_delayable led_work;

/*TODO: Math calculation for led glow */
static uint8_t lerp_u8(uint8_t a, uint8_t b, uint16_t t)
{
	int32_t d = (int32_t)b - (int32_t)a;

	return (uint8_t)(a + (int32_t)((d * (int32_t)t) >> 16));
}

static uint16_t breathing_q16(uint16_t t)
{
	uint32_t x  = (uint32_t)t;
	uint32_t mx = 0xFFFFU - x;

	return (uint16_t)((4U * x * mx) >> 16);
}

static struct ux_led_rgb color_lerp(const struct ux_led_rgb *from, const struct ux_led_rgb *to,
				    uint16_t progress_q16, enum ux_led_transition type)
{
	uint16_t p = (type == UX_LED_TRANSITION_BREATHING) ? breathing_q16(progress_q16)
							   : progress_q16;

	return (struct ux_led_rgb){
		.red = lerp_u8(from->red, to->red, p),
		.green = lerp_u8(from->green, to->green, p),
		.blue = lerp_u8(from->blue, to->blue, p),
	};
}

/* TODO: Need to anlaysis this luma effect*/
static uint8_t rgb_to_luma(const struct ux_led_rgb *c)
{
	return (uint8_t)(((uint32_t)c->r * 2126U + (uint32_t)c->g * 7152U +
	       (uint32_t)c->b *  722U) / 10000U);
}

static void backend_write(const struct ux_led_rgb *color)
{
	uint8_t luma;
	struct ux_led_rgb mono;

	if (!backend_caps.supports_rgb) {
		luma = rgb_to_luma(color);
		mono = { luma, luma, luma };

		ux_led_backend.set_color(&mono);
	} else {
		ux_led_backend.set_color(color);
	}
}

static void led_off(void)
{
	static const struct ux_led_rgb off = {0, 0, 0};

	ux_led_backend.set_color(&off);
}

static void led_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!led_sm.running || sm.pattern == NULL) {
		return;
	}

	struct ux_led_rgb color;
	const struct ux_led_step *next;
	const struct ux_led_step *step = &led_sm.pattern->steps[led_sm.step_idx];
	uint32_t now     = k_uptime_get_32();
	uint32_t remaining;
	uint16_t progress_q16;
	uint32_t elapsed = now - led_sm.step_start_ms;


	if (step->duration_ms == 0U) {
		backend_write(&step->color);
		return;
	}

	if (step->type == UX_LED_TRANSITION_IMMEDIATE) {
		backend_write(&step->color);

		remaining = (elapsed < step->duration_ms) ? (step->duration_ms - elapsed)
								   : 0U;

		if (remaining > 0U) {
			k_work_schedule(&led_work, K_MSEC(remaining));
			return;
		}
	} else {
		if (elapsed < step->duration_ms) {
			progress_q16 = (uint16_t)(((uint32_t)elapsed << 16) /
						step->duration_ms);

			color = color_lerp(&led_sm.step_start_color, &step->color,
							     progress_q16, step->type);

			backend_write(&color);
			k_work_schedule(&led_work, K_MSEC(CONFIG_UX_LED_UPDATE_PERIOD_MS));
			return;
		}
		backend_write(&step->color);
	}

	led_sm.step_start_color = step->color;
	led_sm.step_idx++;

	if (led_sm.step_idx >= sm.pattern->step_cnt) {
		if (led_sm.pattern->loop) {
			led_sm.step_idx = 0;
			LOG_DBG("pattern loop restart");
		} else {
			led_sm.running = false;
			led_sm.pattern = NULL;
			led_off();
			LOG_DBG("pattern complete (no loop)");
			return;
		}
	}

	led_sm.step_start_ms = k_uptime_get_32();

	next = &led_sm.pattern->steps[sm.step_idx];

	if (next->duration_ms == 0U) {
		backend_write(&next->color);
	} else {
		k_work_schedule(&led_work, K_NO_WAIT);
	}
}

static int ux_led_dispatch(const struct ux_event *event)
{
	const struct ux_led_pattern *pattern

	k_work_cancel_delayable(&led_work);

	if (event == NULL || event->led.pattern == NULL) {
		led_sm.running = false;
		led_sm.pattern = NULL;
		led_off();
		LOG_DBG("LED off (queue empty or NULL pattern)");
		return 0;
	}

	pattern= event->led.pattern;

	if (pattern->steps == NULL || pattern->step_cnt == 0U) {
		led_sm.running = false;
		led_sm.pattern = NULL;
		led_off();
		LOG_WRN("invalid pattern (NULL steps or 0 step_cnt)");
		return -EINVAL;
	}

	led_sm.pattern           = pattern;
	led_sm.step_idx          = 0;
	led_sm.step_start_ms     = k_uptime_get_32();
	led_sm.step_start_color  = (struct ux_led_rgb){0, 0, 0};
	led_sm.running           = true;

	LOG_DBG("LED start: steps=%zu loop=%d prio=%u", pattern->step_cnt, pattern->loop,
							event->priority);

	k_work_schedule(&led_work, K_NO_WAIT);

	return 0;
}

/*
 * Led handler register to the UX subsystem.
 */
UX_HANDLER_DEFINE(ux_led_handler, ux_led_dispatch);

static int ux_led_init(void)
{
	int ret = ux_led_backend.init();

	if (ret < 0) {
		LOG_ERR("LED backend init failed: %d", ret);
		return ret;
	}

	ux_led_backend.get_caps(&backend_caps);

	k_work_init_delayable(&led_work, led_work_handler);

	led_sm.running = false;
	led_sm.pattern = NULL;

	LOG_INF("LED handler ready (rgb=%d brightness=%d)", backend_caps.supports_rgb,
		backend_caps.supports_brightness);

	return 0;
}

SYS_INIT(ux_led_init, APPLICATION, CONFIG_UX_LED_INIT_PRIORITY);
