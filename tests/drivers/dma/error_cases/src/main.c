/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#define BUF_SIZE 64
#define TEST_CHANNEL 0
#define INVALID_CHANNEL 0xFF
#define TEST_TIMEOUT K_MSEC(200)

#define DMA_TEST_NODE DT_PATH(zephyr_user)
#define DMA_TEST_DEVS_PROP dma_test_devs

#if DT_NODE_HAS_PROP(DMA_TEST_NODE, DMA_TEST_DEVS_PROP)
#define DMA_DEV DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DMA_TEST_NODE, DMA_TEST_DEVS_PROP, 0))
#else
#define DMA_DEV DEVICE_DT_GET(DT_NODELABEL(tst_dma0))
#endif

static K_SEM_DEFINE(dma_sem, 0, 1);
static int cb_status;
static bool cb_called;

static uint8_t src_buf[BUF_SIZE] __aligned(4);
static uint8_t dst_buf[BUF_SIZE] __aligned(4);

static void dma_test_callback(const struct device *dev, void *user_data,
			      uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	cb_status = status;
	cb_called = true;
	k_sem_give(&dma_sem);
}

static void *dma_error_setup(void)
{
	const struct device *dev = DMA_DEV;

	zassert_true(device_is_ready(dev), "DMA device not ready");

	for (int i = 0; i < BUF_SIZE; i++) {
		src_buf[i] = (uint8_t)i;
	}

	return NULL;
}

static void dma_error_before(void *fixture)
{
	ARG_UNUSED(fixture);

	cb_status = 0;
	cb_called = false;
	k_sem_reset(&dma_sem);
	(void)memset(dst_buf, 0, sizeof(dst_buf));
}

static void dma_error_after(void *fixture)
{
	ARG_UNUSED(fixture);

	dma_stop(DMA_DEV, TEST_CHANNEL);
}

ZTEST(dma_error_cases, test_invalid_channel_config)
{
	const struct device *dev = DMA_DEV;
	struct dma_block_config block = {
		.block_size = BUF_SIZE,
		.source_address = (uint32_t)src_buf,
		.dest_address = (uint32_t)dst_buf,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.head_block = &block,
		.block_count = 1,
	};

	zassert_not_equal(dma_config(dev, INVALID_CHANNEL, &cfg), 0,
			  "dma_config should reject invalid channel");
}

ZTEST(dma_error_cases, test_invalid_channel_start_stop)
{
	const struct device *dev = DMA_DEV;

	zassert_not_equal(dma_start(dev, INVALID_CHANNEL), 0,
			  "dma_start should reject invalid channel");
	zassert_not_equal(dma_stop(dev, INVALID_CHANNEL), 0,
			  "dma_stop should reject invalid channel");
}

ZTEST(dma_error_cases, test_error_callback_enabled)
{
	const struct device *dev = DMA_DEV;
	int ret;

	struct dma_block_config block = {
		.block_size = 16,
		.source_address = (uint32_t)(src_buf + 1),
		.dest_address = (uint32_t)dst_buf,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 4,
		.dest_data_size = 4,
		.source_burst_length = 8,
		.dest_burst_length = 8,
		.dma_callback = dma_test_callback,
		.head_block = &block,
		.block_count = 1,
		.error_callback_dis = 0,
	};

	ret = dma_config(dev, TEST_CHANNEL, &cfg);
	zassert_equal(ret, 0, "dma_config failed: %d", ret);

	ret = dma_start(dev, TEST_CHANNEL);
	zassert_equal(ret, 0, "dma_start failed: %d", ret);

	ret = k_sem_take(&dma_sem, TEST_TIMEOUT);
	zassert_equal(ret, 0, "Error callback was not called within timeout");
	zassert_true(cb_called, "Callback flag not set");
	zassert_true(cb_status < 0, "Expected negative errno, got %d", cb_status);
}

ZTEST(dma_error_cases, test_error_callback_disabled)
{
	const struct device *dev = DMA_DEV;
	int ret;

	struct dma_block_config block = {
		.block_size = 16,
		.source_address = (uint32_t)(src_buf + 1),
		.dest_address = (uint32_t)dst_buf,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 4,
		.dest_data_size = 4,
		.source_burst_length = 8,
		.dest_burst_length = 8,
		.dma_callback = dma_test_callback,
		.head_block = &block,
		.block_count = 1,
		.error_callback_dis = 1,
	};

	ret = dma_config(dev, TEST_CHANNEL, &cfg);
	zassert_equal(ret, 0, "dma_config failed: %d", ret);

	ret = dma_start(dev, TEST_CHANNEL);
	zassert_equal(ret, 0, "dma_start failed: %d", ret);

	ret = k_sem_take(&dma_sem, TEST_TIMEOUT);
	zassert_equal(ret, -EAGAIN, "Callback was called despite error_callback_dis=1");
	zassert_false(cb_called, "Callback should not have been called");
}

ZTEST(dma_error_cases, test_normal_transfer)
{
	const struct device *dev = DMA_DEV;
	int ret;

	struct dma_block_config block = {
		.block_size = BUF_SIZE,
		.source_address = (uint32_t)src_buf,
		.dest_address = (uint32_t)dst_buf,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.source_burst_length = 8,
		.dest_burst_length = 8,
		.dma_callback = dma_test_callback,
		.head_block = &block,
		.block_count = 1,
		.error_callback_dis = 0,
	};

	ret = dma_config(dev, TEST_CHANNEL, &cfg);
	zassert_equal(ret, 0, "dma_config failed: %d", ret);

	ret = dma_start(dev, TEST_CHANNEL);
	zassert_equal(ret, 0, "dma_start failed: %d", ret);

	ret = k_sem_take(&dma_sem, TEST_TIMEOUT);
	zassert_equal(ret, 0, "Transfer callback timed out");
	zassert_true(cb_called, "Callback flag not set");
	zassert_equal(cb_status, 0, "Expected status 0, got %d", cb_status);
	zassert_mem_equal(dst_buf, src_buf, BUF_SIZE, "Buffers do not match");
}

ZTEST_SUITE(dma_error_cases, NULL, dma_error_setup, dma_error_before, dma_error_after, NULL);
