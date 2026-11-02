# Copyright (c) 2026 Aerlync Labs Inc.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smp_i2c_client_dfu)

target_sources(app PRIVATE src/main.c)
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)

# The firmware image this controller uploads to the target is supplied at
# build time via CONFIG_APP_DFU_IMAGE_FILE (a signed MCUboot image, e.g.
# build/zephyr/zephyr.signed.bin), not committed to the tree. When it is unset
# a tiny placeholder is generated so the sample still builds; the placeholder
# is not a usable image.
set(dfu_image_inc ${ZEPHYR_BINARY_DIR}/include/generated/dfu_image.inc)
if(CONFIG_APP_DFU_IMAGE_FILE STREQUAL "")
  message(WARNING
    "CONFIG_APP_DFU_IMAGE_FILE is not set; building with a placeholder image. "
    "Set it to a signed image to perform a real DFU.")
  set(dfu_image_src ${CMAKE_CURRENT_BINARY_DIR}/placeholder_dfu_image.bin)
  file(WRITE ${dfu_image_src} "PLACEHOLDER-IMAGE")
else()
  set(dfu_image_src ${CONFIG_APP_DFU_IMAGE_FILE})
endif()
generate_inc_file_for_target(app ${dfu_image_src} ${dfu_image_inc})
