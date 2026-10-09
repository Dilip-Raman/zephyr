# Copyright (c) 2026 Aerlync Labs Inc.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dma_error_cases)

target_sources(app PRIVATE src/main.c)
