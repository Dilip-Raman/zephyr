/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_UX_UX_LED_H_
#define ZEPHYR_INCLUDE_UX_UX_LED_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _cplusplus
extern "C" {
#endif

/** @brief RGB colour representation. */
struct ux_led_rgb {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

/** @brief Transition type for a pattern step. */
enum ux_led_transition {
	UX_LED_TRANSITION_IMMEDIATE = 0,
	UX_LED_TRANSITION_LINEAR,
	UX_LED_TRANSITION_BREATHING,
};

/**
 * @brief One step in an LED pattern.
 */
struct ux_led_step {
	struct ux_led_rgb	color;
	uint32_t	duration_ms;
	enum ux_led_transition type;
};

/**
 * @brief Complete LED indication pattern.
 */
struct ux_led_pattern {
	const struct ux_led_step *steps;
	size_t step_cnt;
	bool loop;
};

/**
 * @brief Payload for LED events, carried in struct ux_event.led.
 */
struct ux_led_payload {
	const struct ux_led_pattern *pattern;
};

/**
 * @brief LED backend hardware capabilities.
 */
struct ux_led_caps {
	bool supports_rgb;
	bool supports_brightness;
};

/**
 * @brief LED backend driver interface.
 */
struct ux_led_backend_api {
	int (*init)(void);
	int (*set_color)(const struct ux_led_rgb *color);
	void (*get_caps)(struct ux_led_caps *caps);
};

#ifdef _cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_UX_UX_LED_H_ */
