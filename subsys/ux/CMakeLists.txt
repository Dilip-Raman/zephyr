# Copyright (c) 2026 Aerlync Labs Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

# Core dispatcher — always compiled when CONFIG_UX=y.
zephyr_library_sources(ux_notifier.c)

# LED handler and its backends — compiled only when CONFIG_UX_LED=y.
if(CONFIG_UX_LED)
  zephyr_library_sources(led/ux_led_handler.c)

  if(CONFIG_UX_LED_BACKEND_GPIO)
    zephyr_library_sources(led/backends/led_gpio.c)
  endif()
endif()
