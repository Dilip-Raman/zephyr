/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#define I2C_ADDR 0x55

#if DT_NODE_HAS_PROP(DT_NODELABEL(smp_i2c), data_ready_gpios)
static const struct gpio_dt_spec drdy_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(smp_i2c), data_ready_gpios);
#endif

/*
 * The transport exposes its ISR callbacks and tx path to the tests under
 * CONFIG_ZTEST (see SMP_I2C_STATIC in smp_i2c.c). The tests drive the state
 * machine directly rather than through a live I2C controller or the SMP
 * client registry, which the target does not populate.
 */
extern int smp_i2c_write_requested(struct i2c_target_config *cfg);
extern int smp_i2c_write_received(struct i2c_target_config *cfg, uint8_t val);
extern int smp_i2c_read_requested(struct i2c_target_config *cfg, uint8_t *val);
extern int smp_i2c_read_processed(struct i2c_target_config *cfg, uint8_t *val);
extern int smp_i2c_stop(struct i2c_target_config *cfg);
extern int smp_i2c_tx_pkt(struct net_buf *nb);
extern void smp_i2c_test_reset(void);
#if defined(CONFIG_I2C_TARGET_BUFFER_MODE)
extern void smp_i2c_buf_write_received(struct i2c_target_config *cfg, uint8_t *ptr, uint32_t len);
extern int smp_i2c_buf_read_requested(struct i2c_target_config *cfg, uint8_t **ptr, uint32_t *len);
#endif

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	smp_i2c_test_reset();
}

ZTEST_SUITE(smp_i2c, NULL, NULL, before_each, NULL, NULL);

ZTEST(smp_i2c, test_transport_registers)
{
	const struct device *i2c_dev = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(smp_i2c)));

	zassert_true(device_is_ready(i2c_dev), "I2C device not ready");
}

ZTEST(smp_i2c, test_drdy_starts_deasserted)
{
#if DT_NODE_HAS_PROP(DT_NODELABEL(smp_i2c), data_ready_gpios)
	zassert_true(device_is_ready(drdy_gpio.port), "DRDY GPIO device not ready");
	int val = gpio_pin_get_dt(&drdy_gpio);

	zassert_equal(val, 0, "DRDY should be deasserted initially");
#else
	ztest_test_skip();
#endif
}

ZTEST(smp_i2c, test_zero_length_poll)
{
	uint8_t val = 0xFF;

	smp_i2c_read_requested(NULL, &val);
	zassert_equal(val, 0x00, "Expected 0x00 for zero-length poll");
	smp_i2c_read_processed(NULL, &val);
	zassert_equal(val, 0x00, "Expected 0x00 for next byte");
	smp_i2c_stop(NULL);
}

ZTEST(smp_i2c, test_rx_state_machine_valid_frame)
{
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	size_t before = smp_packet_buffers_available();
#endif
	smp_i2c_write_requested(NULL);
	smp_i2c_write_received(NULL, 0x00);
	smp_i2c_write_received(NULL, 0x05);
	for (int i = 0; i < 5; i++) {
		smp_i2c_write_received(NULL, i);
	}
	smp_i2c_stop(NULL); /* hands a complete frame to the SMP workqueue */

	/*
	 * Let the cooperative workqueue thread process (and free) the
	 * dispatched request, then confirm every net_buf returned to the pool.
	 */
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	k_msleep(100);
	zassert_equal(smp_packet_buffers_available(), before,
		      "valid frame processing leaked a net_buf");
#endif
}

ZTEST(smp_i2c, test_rx_state_machine_truncated_frame)
{
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	size_t before = smp_packet_buffers_available();
#endif
	smp_i2c_write_requested(NULL);
	smp_i2c_write_received(NULL, 0x00);
	smp_i2c_write_received(NULL, 0x0A); /* declares 10 bytes */
	for (int i = 0; i < 5; i++) {
		smp_i2c_write_received(NULL, i); /* only 5 arrive */
	}
	smp_i2c_stop(NULL); /* truncated: must be dropped */
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	zassert_equal(smp_packet_buffers_available(), before,
		      "truncated frame must not leak a net_buf");
#endif
}

ZTEST(smp_i2c, test_rx_state_machine_zero_length_rejected)
{
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	size_t before = smp_packet_buffers_available();
#endif
	smp_i2c_write_requested(NULL);
	smp_i2c_write_received(NULL, 0x00);
	smp_i2c_write_received(NULL, 0x00); /* length 0: rejected before alloc */
	smp_i2c_stop(NULL);
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	zassert_equal(smp_packet_buffers_available(), before,
		      "zero-length frame must not allocate a net_buf");
#endif
}

ZTEST(smp_i2c, test_rx_state_machine_oversize_rejected)
{
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	size_t before = smp_packet_buffers_available();
#endif
	smp_i2c_write_requested(NULL);
	smp_i2c_write_received(NULL, 0xFF); /* length 0xFFFF > MTU */
	smp_i2c_write_received(NULL, 0xFF);
	smp_i2c_stop(NULL);
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_AVAILABLE
	zassert_equal(smp_packet_buffers_available(), before,
		      "oversize frame must not leak a net_buf");
#endif
}

ZTEST(smp_i2c, test_tx_path_stages_response)
{
	struct net_buf *nb = smp_packet_alloc();

	zassert_not_null(nb, "net_buf alloc failed");

	uint8_t payload[] = {0xAA, 0xBB, 0xCC};

	net_buf_add_mem(nb, payload, sizeof(payload));

	int rc = smp_i2c_tx_pkt(nb);

	zassert_equal(rc, 0, "tx_pkt failed");

#if DT_NODE_HAS_PROP(DT_NODELABEL(smp_i2c), data_ready_gpios)
	int val = gpio_pin_get_dt(&drdy_gpio);

	zassert_equal(val, 1, "DRDY should be asserted after tx_pkt");
#endif

	uint8_t val0 = 0, val1 = 0;

	smp_i2c_read_requested(NULL, &val0);
	smp_i2c_read_processed(NULL, &val1);
	zassert_equal(val0, 0x00, "Length high should be 0");
	zassert_equal(val1, 0x03, "Length low should be 3");

	uint8_t data[3] = {0};

	smp_i2c_read_processed(NULL, &data[0]);
	smp_i2c_read_processed(NULL, &data[1]);
	smp_i2c_read_processed(NULL, &data[2]);

	zassert_mem_equal(data, payload, 3, "Payload mismatch");

	smp_i2c_stop(NULL);
}

/*
 * A READ that begins while no response is staged must serve 0x00 for its
 * entire life, even if the workqueue stages a response (here a 256-byte one,
 * so a torn length high byte would surface as non-zero) before the next ISR
 * byte is served.
 */
ZTEST(smp_i2c, test_tx_not_ready_read_stays_zero)
{
	uint8_t v = 0xFF;

	smp_i2c_read_requested(NULL, &v);
	zassert_equal(v, 0x00, "byte0 must be 0x00 when no response is staged");

	struct net_buf *nb = smp_packet_alloc();

	zassert_not_null(nb, "alloc failed");
	for (int i = 0; i < 256; i++) {
		net_buf_add_u8(nb, (uint8_t)i); /* len 256 -> len_hi = 0x01 */
	}
	zassert_equal(smp_i2c_tx_pkt(nb), 0, "staging 256-byte response failed");

	v = 0xFF;
	smp_i2c_read_processed(NULL, &v);
	zassert_equal(v, 0x00, "byte1 must stay 0x00 (no torn length header)");
	v = 0xFF;
	smp_i2c_read_processed(NULL, &v);
	zassert_equal(v, 0x00, "later bytes of a not-ready read stay 0x00");

	smp_i2c_stop(NULL);
}

/*
 * A response staged while a different READ is in flight (tx_reading set, no
 * stop() yet) must be rejected with -EBUSY rather than corrupting the buffer
 * that read is draining.
 */
ZTEST(smp_i2c, test_tx_pkt_rejected_while_read_in_flight)
{
	struct net_buf *nb1 = smp_packet_alloc();
	uint8_t payload1[] = {0x11, 0x22, 0x33};

	net_buf_add_mem(nb1, payload1, sizeof(payload1));
	zassert_equal(smp_i2c_tx_pkt(nb1), 0, "first tx_pkt failed");

	uint8_t val = 0;

	smp_i2c_read_requested(NULL, &val); /* READ now in flight (tx_reading) */
	zassert_equal(val, 0x00, "length high mismatch");

	struct net_buf *nb2 = smp_packet_alloc();
	uint8_t payload2[] = {0x44, 0x55};

	net_buf_add_mem(nb2, payload2, sizeof(payload2));
	zassert_equal(smp_i2c_tx_pkt(nb2), -EBUSY, "expected -EBUSY while a read is in flight");

	smp_i2c_read_processed(NULL, &val);
	zassert_equal(val, 0x03, "length low mismatch after rejected overwrite");

	uint8_t data[3] = {0};

	smp_i2c_read_processed(NULL, &data[0]);
	smp_i2c_read_processed(NULL, &data[1]);
	smp_i2c_read_processed(NULL, &data[2]);
	zassert_mem_equal(data, payload1, 3, "in-flight payload was corrupted");

	smp_i2c_stop(NULL);
}

ZTEST(smp_i2c, test_echo_round_trip)
{
#ifdef CONFIG_I2C_EMUL
	const struct device *i2c_dev = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(smp_i2c)));

	uint8_t write_buf[] = {0x00, 0x02, 0x11, 0x22};
	int rc = i2c_write(i2c_dev, write_buf, sizeof(write_buf), I2C_ADDR);

	zassert_equal(rc, 0, "i2c_write failed");
#else
	ztest_test_skip();
#endif
}

ZTEST(smp_i2c, test_mtu_boundary_exact_mtu)
{
	struct net_buf *nb = smp_packet_alloc();
	uint16_t mtu = CONFIG_MCUMGR_TRANSPORT_I2C_MTU;

	for (int i = 0; i < mtu; i++) {
		net_buf_add_u8(nb, i & 0xFF);
	}
	int rc = smp_i2c_tx_pkt(nb);

	zassert_equal(rc, 0, "MTU exact size should be accepted");
	smp_i2c_stop(NULL);
}

ZTEST(smp_i2c, test_mtu_boundary_oversize)
{
	struct net_buf *nb = smp_packet_alloc();
	uint16_t mtu = CONFIG_MCUMGR_TRANSPORT_I2C_MTU;

	for (int i = 0; i < mtu + 1; i++) {
		net_buf_add_u8(nb, i & 0xFF);
	}
	int rc = smp_i2c_tx_pkt(nb);

	zassert_equal(rc, -EMSGSIZE, "MTU+1 should be rejected");
}

#if defined(CONFIG_I2C_TARGET_BUFFER_MODE)
/*
 * In buffer mode the controller reads twice (the length, then the full
 * frame). The staged response must survive the first read and only be
 * dropped when the next request arrives.
 */
ZTEST(smp_i2c, test_buffer_mode_latched_until_next_request)
{
	struct net_buf *nb = smp_packet_alloc();
	uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

	net_buf_add_mem(nb, payload, sizeof(payload));
	zassert_equal(smp_i2c_tx_pkt(nb), 0, "staging response failed");

	uint8_t *p1 = NULL;
	uint32_t l1 = 0;

	smp_i2c_buf_read_requested(NULL, &p1, &l1);
	zassert_equal(l1, 2 + sizeof(payload), "first read frame length");
	zassert_equal(p1[0], 0x00, "length high");
	zassert_equal(p1[1], sizeof(payload), "length low");
	zassert_mem_equal(&p1[2], payload, sizeof(payload), "first read payload");

	/* Second read must return the same frame, not zeros. */
	uint8_t *p2 = NULL;
	uint32_t l2 = 0;

	smp_i2c_buf_read_requested(NULL, &p2, &l2);
	zassert_equal(l2, 2 + sizeof(payload), "second read must repeat the frame");
	zassert_mem_equal(&p2[2], payload, sizeof(payload),
			  "second read must return the same payload");

	/* The next request discards the staged response. */
	uint8_t req[] = {0x00, 0x01, 0x42};

	smp_i2c_buf_write_received(NULL, req, sizeof(req));

	uint8_t *p3 = NULL;
	uint32_t l3 = 0;

	smp_i2c_buf_read_requested(NULL, &p3, &l3);
	zassert_equal(l3, 2, "after the next request a read returns the idle prefix");
	zassert_equal(p3[0], 0x00, "idle length high");
	zassert_equal(p3[1], 0x00, "idle length low");
}
#endif /* CONFIG_I2C_TARGET_BUFFER_MODE */

/* Provide a dummy device to satisfy i2c_emul.c, which expects all children
 * to be devices. A single zephyr,smp-i2c node is supported (see the
 * BUILD_ASSERT in smp_i2c.c).
 */
#define DT_DRV_COMPAT zephyr_smp_i2c
#if DT_NODE_HAS_COMPAT(DT_DRV_INST(0), zephyr_smp_i2c)
DEVICE_DT_DEFINE(DT_DRV_INST(0), NULL, NULL, NULL, NULL, POST_KERNEL, 99, NULL);
#endif
