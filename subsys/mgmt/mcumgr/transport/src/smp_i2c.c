/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/net_buf.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/spinlock.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(smp_i2c, CONFIG_MCUMGR_TRANSPORT_I2C_LOG_LEVEL);

BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_I2C_MTU > 0);
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_I2C_MTU <= CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE,
	     "I2C MTU must not exceed net_buf size");

/* 2-byte big-endian packet length prefix on the wire */
#define SMP_I2C_LEN_FIELD   2U
#define SMP_I2C_TX_BUF_SIZE (SMP_I2C_LEN_FIELD + CONFIG_MCUMGR_TRANSPORT_I2C_MTU)

/*
 * The alias "smp-i2c-bus" in the board overlay points either directly to the
 * I2C peripheral node or to a standalone "zephyr,smp-i2c-transport" node.
 */
#define SMP_I2C_ALIAS_NODE DT_ALIAS(smp_i2c_bus)

#define SMP_I2C_BUS_NODE \
	COND_CODE_1(DT_NODE_HAS_PROP(SMP_I2C_ALIAS_NODE, i2c_bus), \
		    (DT_PHANDLE(SMP_I2C_ALIAS_NODE, i2c_bus)), \
		    (SMP_I2C_ALIAS_NODE))

static const struct device *const i2c_dev = DEVICE_DT_GET(SMP_I2C_BUS_NODE);

#if DT_NODE_HAS_PROP(SMP_I2C_ALIAS_NODE, data_ready_gpios)
#define SMP_I2C_HAS_DRDY 1
static const struct gpio_dt_spec drdy_gpio =
	GPIO_DT_SPEC_GET(SMP_I2C_ALIAS_NODE, data_ready_gpios);
#else
#define SMP_I2C_HAS_DRDY 0
#endif

/*
 * RX state machine
 *
 * I2C target callbacks run in ISR context. Each incoming WRITE transaction
 * carries [2B BE length][SMP packet]. The state machine tracks which part
 * of that framing we are currently consuming byte-by-byte.
 *
 */
enum smp_i2c_rx_state {
	SMP_I2C_RX_LEN_HIGH,
	SMP_I2C_RX_LEN_LOW,
	SMP_I2C_RX_DATA,
	SMP_I2C_RX_ERROR,
};

static enum smp_i2c_rx_state rx_state;
static uint16_t              rx_expected;
static uint16_t              rx_received;
static struct net_buf       *rx_nb;

/*
 * TX buffer
 *
 * Shared between the SMP workqueue (smp_i2c_tx_pkt , writes it) and the I2C
 * ISR (read_requested / read_processed , reads it). A spinlock is used because
 * mutexes cannot be acquired from ISR context.
 */
static struct k_spinlock tx_lock;

static uint8_t  tx_buf[SMP_I2C_TX_BUF_SIZE];
static uint16_t tx_len;
static uint16_t tx_pos;
static bool     tx_ready;

static struct smp_transport smp_i2c_transport;

static inline void drdy_set(int val)
{
#if SMP_I2C_HAS_DRDY
	gpio_pin_set_dt(&drdy_gpio, val);
#else
	ARG_UNUSED(val);
#endif
}

/*
 * I2C target callbacks - ISR context
 */

static int smp_i2c_write_requested(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);

	if (rx_nb != NULL) {
		smp_packet_free(rx_nb);
		rx_nb = NULL;
	}

	rx_state    = SMP_I2C_RX_LEN_HIGH;
	rx_expected = 0;
	rx_received = 0;

	return 0;
}

static int smp_i2c_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	ARG_UNUSED(cfg);

	switch (rx_state) {

	case SMP_I2C_RX_LEN_HIGH:
		rx_expected = (uint16_t)val << 8;
		rx_state    = SMP_I2C_RX_LEN_LOW;
		break;

	case SMP_I2C_RX_LEN_LOW:
		rx_expected |= val;

		if (rx_expected == 0 ||
		    rx_expected > CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE) {
			LOG_ERR("bad frame len %u", rx_expected);
			rx_state = SMP_I2C_RX_ERROR;
			break;
		}

		rx_nb = smp_packet_alloc();
		if (rx_nb == NULL) {
			LOG_ERR("net_buf alloc failed");
			rx_state = SMP_I2C_RX_ERROR;
			break;
		}

		rx_state = SMP_I2C_RX_DATA;
		break;

	case SMP_I2C_RX_DATA:
		if (net_buf_tailroom(rx_nb) == 0) {
			LOG_ERR("net_buf overflow");
			smp_packet_free(rx_nb);
			rx_nb    = NULL;
			rx_state = SMP_I2C_RX_ERROR;
			break;
		}

		net_buf_add_u8(rx_nb, val);
		rx_received++;
		break;

	case SMP_I2C_RX_ERROR:
		break;
	}

	return 0;
}

static int smp_i2c_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);

	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	*val  = tx_ready ? tx_buf[0] : 0x00;
	tx_pos = 1;

	k_spin_unlock(&tx_lock, key);
	return 0;
}

static int smp_i2c_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);

	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	uint16_t total = tx_ready ? (SMP_I2C_LEN_FIELD + tx_len) : SMP_I2C_LEN_FIELD;

	*val = (tx_pos < total) ? tx_buf[tx_pos++] : 0x00;

	k_spin_unlock(&tx_lock, key);
	return 0;
}

static int smp_i2c_stop(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);

	/* End of a WRITE: dispatch a complete packet, drop an incomplete one */
	if (rx_state == SMP_I2C_RX_DATA && rx_nb != NULL) {
		if (rx_received == rx_expected) {
			smp_rx_req(&smp_i2c_transport, rx_nb);
		} else {
			LOG_WRN("truncated frame %u/%u", rx_received, rx_expected);
			smp_packet_free(rx_nb);
		}
		rx_nb = NULL;
	} else if (rx_nb != NULL) {
		smp_packet_free(rx_nb);
		rx_nb = NULL;
	}

	rx_state    = SMP_I2C_RX_LEN_HIGH;
	rx_expected = 0;
	rx_received = 0;

	/* End of a READ: de-assert DRDY once the controller read everything */
	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	if (tx_ready && tx_pos >= (SMP_I2C_LEN_FIELD + tx_len)) {
		tx_ready = false;
		tx_len   = 0;
		tx_pos   = 0;
		k_spin_unlock(&tx_lock, key);

		drdy_set(0);
		LOG_DBG("response consumed");
		return 0;
	}

	k_spin_unlock(&tx_lock, key);
	return 0;
}

static const struct i2c_target_callbacks smp_i2c_callbacks = {
	.write_requested = smp_i2c_write_requested,
	.write_received  = smp_i2c_write_received,
	.read_requested  = smp_i2c_read_requested,
	.read_processed  = smp_i2c_read_processed,
	.stop            = smp_i2c_stop,
};

static struct i2c_target_config smp_i2c_target_cfg = {
	.address   = CONFIG_MCUMGR_TRANSPORT_I2C_ADDR,
	.callbacks = &smp_i2c_callbacks,
};

/*
 * SMP transport callbacks
 */

static int smp_i2c_tx_pkt(struct net_buf *nb)
{
	uint16_t pkt_len = nb->len;

	if (pkt_len > CONFIG_MCUMGR_TRANSPORT_I2C_MTU) {
		LOG_ERR("response too large: %u > MTU %u",
			pkt_len, CONFIG_MCUMGR_TRANSPORT_I2C_MTU);
		smp_packet_free(nb);
		return MGMT_ERR_EMSGSIZE;
	}

	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	if (tx_ready) {
		LOG_WRN("overwriting unread response");
	}

	tx_buf[0] = (uint8_t)(pkt_len >> 8);
	tx_buf[1] = (uint8_t)(pkt_len & 0xFFU);
	memcpy(&tx_buf[SMP_I2C_LEN_FIELD], nb->data, pkt_len);
	tx_len  = pkt_len;
	tx_pos  = 0;
	tx_ready = true;

	k_spin_unlock(&tx_lock, key);

	drdy_set(1);

	LOG_DBG("buffered %u byte response", pkt_len);
	smp_packet_free(nb);

	return 0;
}

static uint16_t smp_i2c_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	return CONFIG_MCUMGR_TRANSPORT_I2C_MTU;
}

static void smp_i2c_start(void)
{
	int rc;

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C device not ready");
		return;
	}

#if SMP_I2C_HAS_DRDY
	if (!gpio_is_ready_dt(&drdy_gpio)) {
		LOG_ERR("DRDY GPIO not ready");
		return;
	}

	rc = gpio_pin_configure_dt(&drdy_gpio, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		LOG_ERR("DRDY configure failed: %d", rc);
		return;
	}
#endif

	smp_i2c_transport.functions = (struct smp_transport_api_t){
		.output  = smp_i2c_tx_pkt,
		.get_mtu = smp_i2c_get_mtu,
	};

	rc = smp_transport_init(&smp_i2c_transport);
	if (rc != 0) {
		LOG_ERR("smp_transport_init failed: %d", rc);
		return;
	}

	rc = i2c_target_register(i2c_dev, &smp_i2c_target_cfg);
	if (rc != 0) {
		LOG_ERR("i2c_target_register failed: %d", rc);
	}
}

MCUMGR_HANDLER_DEFINE(smp_i2c, smp_i2c_start);
