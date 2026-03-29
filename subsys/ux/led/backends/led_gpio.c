/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO LED backend for the UX LED handler.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/ux/ux_led.h>

LOG_MODULE_REGISTER(ux_led_gpio, CONFIG_UX_LOG_LEVEL);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static int gpio_backend_set_color(const struct ux_led_rgb *color)
{
	bool led_on = (color->red > 0U || color->green > 0U || color->blue > 0U);

	return gpio_pin_set_dt(&led, led_on ? 1 : 0);
}

/* TODO: Need to add more feature in it to get capabilities */
static void gpio_backend_get_caps(struct ux_led_caps *caps)
{
	caps->supports_rgb        = false;
	caps->supports_brightness = false;
}

/*
 * @brief GPIO backend support enable api's.
 */
static int gpio_backend_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (ret < 0) {
		LOG_ERR("GPIO configure failed: %d", ret);
	}

	return ret;
}
/*
 *@brief payload for the gpio backend structure to the led.
 */
const struct ux_led_backend_api ux_led_backend = {
	.init      = gpio_backend_init,
	.set_color = gpio_backend_set_color,
	.get_caps  = gpio_backend_get_caps,
};
