/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_UX_UX_EVENT_H_
#define ZEPHYR_INCLUDE_UX_UX_EVENT_H_

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/ux/ux_led.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UX event structure.
 *
 * Holds the different payload of the module to para's the structure
 */
struct ux_event {
	uint8_t priority;

	union {
		struct ux_led_payload led;
	};
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_UX_UX_EVENT_H_ */
