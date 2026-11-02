/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief I2C target transport for the MCUmgr SMP protocol.
 *
 * WRITE carries an SMP command framed as a 2-byte big-endian length prefix
 * then the raw SMP packet; READ returns a response with the same prefix, or
 * [0x00][0x00] while idle. An optional data-ready GPIO replaces polling.
 *
 * Byte-mode and buffer-mode (DMA) target drivers are both supported. On a
 * buffer-mode driver that never calls stop() after a READ (Nordic TWIS) a
 * staged response is held until the next WRITE rather than cleared when a
 * read starts, so the controller can read the length then the frame.
 *
 * One WRITE carries one SMP packet; there is no reassembly. The target stays
 * registered for the application lifetime (no PM_DEVICE or unregister).
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

LOG_MODULE_DECLARE(mcumgr_smp, CONFIG_MCUMGR_TRANSPORT_LOG_LEVEL);

/*
 * The I2C target ISR callbacks and smp_i2c_tx_pkt() are file-local in normal
 * builds. Under CONFIG_ZTEST they are given external linkage so the unit
 * tests can drive the protocol state machine directly, without a live I2C
 * controller or the SMP client registry.
 */
#ifdef CONFIG_ZTEST
#define SMP_I2C_STATIC
#else
#define SMP_I2C_STATIC static
#endif

#define DT_DRV_COMPAT zephyr_smp_i2c

/* Wire framing: [len_high][len_low][SMP payload...] */
#define SMP_I2C_LEN_FIELD   2U
/* Length prefix plus the SMP payload; in buffer mode this is also the
 * minimum DMA buffer the target driver must provide (see Kconfig.i2c).
 */
#define SMP_I2C_TX_BUF_SIZE (SMP_I2C_LEN_FIELD + CONFIG_MCUMGR_TRANSPORT_I2C_MTU)

/*
 * Module-level state ties this transport to a single node. Supporting a
 * target on one bus and a controller on another needs per-instance data and
 * is tracked as a follow-up; fail the build loudly until then.
 */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Only one zephyr,smp-i2c node is supported");

BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_I2C_MTU <= CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE,
	     "CONFIG_MCUMGR_TRANSPORT_I2C_MTU must not exceed "
	     "CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE");

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_BUS(DT_DRV_INST(0)));

#if DT_INST_NODE_HAS_PROP(0, data_ready_gpios)
#define SMP_I2C_HAS_DRDY 1
static const struct gpio_dt_spec drdy_gpio = GPIO_DT_SPEC_INST_GET(0, data_ready_gpios);
#else
#define SMP_I2C_HAS_DRDY 0
#endif

/* RX state machine for byte mode. Buffer mode parses the frame in one shot. */
enum smp_i2c_rx_state {
	SMP_I2C_RX_LEN_HIGH,
	SMP_I2C_RX_LEN_LOW,
	SMP_I2C_RX_DATA,
	SMP_I2C_RX_ERROR,
};

static enum smp_i2c_rx_state rx_state;
static uint16_t rx_expected;
static uint16_t rx_received;
static struct net_buf *rx_nb;

/*
 * TX state, shared between the workqueue thread (smp_i2c_tx_pkt) and the ISR
 * callbacks. A k_spinlock is required because a k_mutex cannot be locked from
 * an ISR.
 *
 * tx_buf is the DMA source buffer in buffer mode, so it must be in RAM. A
 * staged response (tx_ready) is consumed by the next WRITE, not by a READ
 * starting: the controller serialises request/response, so a fresh WRITE
 * proves the previous response was read. tx_reading marks that the current
 * READ transaction is draining tx_buf; a READ that begins while !tx_ready
 * keeps tx_reading false and serves zeros for its whole life, so a response
 * staged mid-transaction can never tear an in-progress read's bytes.
 */
static struct k_spinlock tx_lock;
static uint8_t tx_buf[SMP_I2C_TX_BUF_SIZE];
static uint16_t tx_len; /* SMP packet bytes staged in tx_buf (prefix is 2 more) */
static uint16_t tx_pos; /* next tx_buf index for the current READ */
static bool tx_ready;   /* a response is staged in tx_buf */
static bool tx_reading; /* the current READ transaction is draining tx_buf */

/* Caller holds tx_lock. Drop any staged response; returns true if one was staged. */
static bool tx_discard_locked(void)
{
	bool was_ready = tx_ready;

	tx_ready = false;
	tx_reading = false;
	tx_len = 0U;
	tx_pos = 0U;

	return was_ready;
}

#if defined(CONFIG_I2C_TARGET_BUFFER_MODE)
/*
 * Returned by buf_read_requested when no response is pending. Must be in
 * RAM, because DMA cannot source from .rodata on platforms like nRF52840.
 */
static uint8_t zero_buf[SMP_I2C_LEN_FIELD];
#endif

static struct smp_transport smp_i2c_transport;

/*
 * Drive the data-ready GPIO. Called from the I2C target ISR, so the line must
 * be an on-SoC GPIO whose set does not sleep (no off-chip expander).
 */
static inline void drdy_set(int val)
{
#if SMP_I2C_HAS_DRDY
	(void)gpio_pin_set_dt(&drdy_gpio, val);
#else
	ARG_UNUSED(val);
#endif
}

/* Byte-mode ISR callbacks */

SMP_I2C_STATIC int smp_i2c_write_requested(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	k_spinlock_key_t key;
	bool drdy_clear = false;

	/* Discard any partial frame left by an interrupted transaction. */
	if (rx_nb != NULL) {
		smp_packet_free(rx_nb);
		rx_nb = NULL;
	}

	rx_state = SMP_I2C_RX_LEN_HIGH;
	rx_expected = 0U;
	rx_received = 0U;

	/*
	 * An I2C target cannot receive a WRITE while transmitting a READ, so
	 * a new WRITE proves any prior READ has ended and any staged response
	 * was consumed. Drop it here.
	 */
	key = k_spin_lock(&tx_lock);
	drdy_clear = tx_discard_locked();
	k_spin_unlock(&tx_lock, key);

	if (drdy_clear != false) {
		drdy_set(0);
	}

	return 0;
}

SMP_I2C_STATIC int smp_i2c_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	ARG_UNUSED(cfg);

	switch (rx_state) {
	case SMP_I2C_RX_LEN_HIGH:
		rx_expected = (uint16_t)val << 8;
		rx_state = SMP_I2C_RX_LEN_LOW;
		break;

	case SMP_I2C_RX_LEN_LOW:
		rx_expected |= val;

		if (rx_expected == 0U || rx_expected > CONFIG_MCUMGR_TRANSPORT_I2C_MTU) {
			rx_state = SMP_I2C_RX_ERROR;
			break;
		}

		rx_nb = smp_packet_alloc();
		if (rx_nb == NULL) {
			rx_state = SMP_I2C_RX_ERROR;
			break;
		}

		rx_state = SMP_I2C_RX_DATA;
		break;

	case SMP_I2C_RX_DATA:
		/* Reject more bytes than the length prefix declared. */
		if (rx_received >= rx_expected || net_buf_tailroom(rx_nb) == 0U) {
			smp_packet_free(rx_nb);
			rx_nb = NULL;
			rx_state = SMP_I2C_RX_ERROR;
			break;
		}

		net_buf_add_u8(rx_nb, val);
		rx_received++;
		break;

	case SMP_I2C_RX_ERROR:
		/* Drain remaining bytes until STOP resets the state machine. */
		break;
	}

	return 0;
}

SMP_I2C_STATIC int smp_i2c_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	k_spinlock_key_t key;

	ARG_UNUSED(cfg);

	key = k_spin_lock(&tx_lock);
	tx_reading = tx_ready;
	tx_pos = 0U;
	*val = tx_reading ? tx_buf[tx_pos++] : 0x00U;
	k_spin_unlock(&tx_lock, key);

	return 0;
}

SMP_I2C_STATIC int smp_i2c_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	k_spinlock_key_t key;

	ARG_UNUSED(cfg);

	key = k_spin_lock(&tx_lock);
	if (tx_reading && tx_pos < (SMP_I2C_LEN_FIELD + tx_len)) {
		*val = tx_buf[tx_pos++];
	} else {
		*val = 0x00U;
	}
	k_spin_unlock(&tx_lock, key);

	return 0;
}

/* @brief stop() fires at the end of every I2C transaction (WRITE or READ).
 *
 * WRITE end: dispatch a complete frame to the SMP workqueue, or free a
 *            partial/errored one.
 * READ end:  discard the staged response and deassert DRDY once the
 *            controller has clocked all bytes (tx_pos reached the end of the
 *            buffer). Byte mode only.
 *
 * Nordic TWIS does not call stop() after a READ. In buffer mode the staged
 * response is instead held until the next request discards it in
 * smp_i2c_buf_write_received().
 */
SMP_I2C_STATIC int smp_i2c_stop(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	k_spinlock_key_t key;
	bool drdy_clear = false;

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

	rx_state = SMP_I2C_RX_LEN_HIGH;
	rx_expected = 0U;
	rx_received = 0U;

	key = k_spin_lock(&tx_lock);

	if (tx_reading && tx_pos >= (SMP_I2C_LEN_FIELD + tx_len)) {
		/* Controller clocked the whole frame out. */
		drdy_clear = tx_discard_locked();
	}
	tx_reading = false;

	k_spin_unlock(&tx_lock, key);

	if (drdy_clear != false) {
		drdy_set(0);
		LOG_DBG("response consumed");
	}

	return 0;
}

/* Buffer-mode ISR callbacks */

#if defined(CONFIG_I2C_TARGET_BUFFER_MODE)

SMP_I2C_STATIC void smp_i2c_buf_write_received(struct i2c_target_config *cfg, uint8_t *ptr,
					       uint32_t len)
{
	ARG_UNUSED(cfg);
	uint16_t smp_len;
	struct net_buf *nb;
	k_spinlock_key_t key;
	bool drdy_clear;

	/*
	 * A new request proves the controller finished with any staged
	 * response (it serialises request/response), so drop it now;
	 * otherwise a later READ would replay a stale frame. This is the
	 * consumption point in buffer mode, where stop() is not called after
	 * a READ on TWIS.
	 */
	key = k_spin_lock(&tx_lock);
	drdy_clear = tx_discard_locked();
	k_spin_unlock(&tx_lock, key);
	if (drdy_clear != false) {
		drdy_set(0);
	}

	if (len < SMP_I2C_LEN_FIELD) {
		LOG_WRN("frame too short: %u", len);
		return;
	}

	smp_len = ((uint16_t)ptr[0] << 8) | ptr[1];

	if (smp_len == 0U || smp_len > CONFIG_MCUMGR_TRANSPORT_I2C_MTU) {
		LOG_ERR("invalid frame length %u", smp_len);
		return;
	}

	if (len < (uint32_t)(SMP_I2C_LEN_FIELD + smp_len)) {
		/*
		 * Also the safety net for an undersized vendor DMA buffer
		 * (see the buffer-mode sizing note at the top of this file):
		 * a controller driver whose hardware buffer is smaller than
		 * MTU + 2 will hand us fewer bytes than the frame declares,
		 * and we drop it here instead of reading out of bounds.
		 */
		LOG_WRN("frame truncated: got %u expected %u", len,
			(uint32_t)(SMP_I2C_LEN_FIELD + smp_len));
		return;
	}

	nb = smp_packet_alloc();
	if (nb == NULL) {
		LOG_ERR("net_buf alloc failed");
		return;
	}

	net_buf_add_mem(nb, &ptr[SMP_I2C_LEN_FIELD], smp_len);
	smp_rx_req(&smp_i2c_transport, nb);
}

SMP_I2C_STATIC int smp_i2c_buf_read_requested(struct i2c_target_config *cfg, uint8_t **ptr,
					      uint32_t *len)
{
	ARG_UNUSED(cfg);
	k_spinlock_key_t key;
	bool do_drdy_clear = false;

	key = k_spin_lock(&tx_lock);

	if (tx_ready == true) {
		*ptr = tx_buf;
		*len = SMP_I2C_LEN_FIELD + tx_len;
		/*
		 * Keep the response latched. TWIS copies tx_buf into its DMA
		 * buffer synchronously here, but it never calls stop() after a
		 * READ and the controller re-reads the frame (it polls the
		 * 2-byte length, then reads the whole frame). The frame is
		 * dropped only when the next request arrives
		 * (buf_write_received). DRDY is an edge hint, so deassert it
		 * on the first read.
		 */
		do_drdy_clear = true;
	} else {
		*ptr = zero_buf;
		*len = SMP_I2C_LEN_FIELD;
	}

	k_spin_unlock(&tx_lock, key);

	/* Deassert DRDY outside the spinlock to keep the ISR critical section
	 * short (see drdy_set: the line must be an on-SoC GPIO).
	 */
	if (do_drdy_clear == true) {
		drdy_set(0);
	}

	return 0;
}

#endif /* CONFIG_I2C_TARGET_BUFFER_MODE */

/* Each I2C target driver uses only the callback set it supports. */
static const struct i2c_target_callbacks smp_i2c_callbacks = {
	.write_requested = smp_i2c_write_requested,
	.write_received = smp_i2c_write_received,
	.read_requested = smp_i2c_read_requested,
	.read_processed = smp_i2c_read_processed,
	.stop = smp_i2c_stop,
#if defined(CONFIG_I2C_TARGET_BUFFER_MODE)
	.buf_write_received = smp_i2c_buf_write_received,
	.buf_read_requested = smp_i2c_buf_read_requested,
#endif
};

static struct i2c_target_config smp_i2c_target_cfg = {
	.address = DT_REG_ADDR(DT_DRV_INST(0)),
	.callbacks = &smp_i2c_callbacks,
};

/* Thread-context SMP transport callbacks */

/*
 * smp_i2c_tx_pkt() is called by the MCUmgr workqueue to stage a response.
 *
 * Refuses with -EBUSY while tx_reading is set (a READ is draining tx_buf in
 * byte mode) rather than corrupting the in-flight transfer; the SMP client
 * retries per CONFIG_SMP_CMD_RETRY_TIME. In buffer mode tx_reading is never
 * set: the controller serialises request/response and the staged frame is
 * untouched until the next request, so staging is always safe there.
 */
SMP_I2C_STATIC int smp_i2c_tx_pkt(struct net_buf *nb)
{
	k_spinlock_key_t key;
	uint16_t pkt_len = nb->len;

	if (pkt_len > CONFIG_MCUMGR_TRANSPORT_I2C_MTU) {
		LOG_ERR("response too large: %u > MTU %u", pkt_len,
			(uint32_t)CONFIG_MCUMGR_TRANSPORT_I2C_MTU);
		smp_packet_free(nb);
		return -EMSGSIZE;
	}

	key = k_spin_lock(&tx_lock);

	if (tx_reading) {
		/*
		 * A READ is clocking bytes out of tx_buf right now (byte
		 * mode); clobbering it would tear the active transfer. Queue
		 * depth of one is an accepted limitation of this transport.
		 */
		k_spin_unlock(&tx_lock, key);
		LOG_WRN("read in progress; dropping new response");
		smp_packet_free(nb);
		return -EBUSY;
	}

	tx_buf[0] = (uint8_t)(pkt_len >> 8);
	tx_buf[1] = (uint8_t)(pkt_len & 0xFFU);
	memcpy(&tx_buf[SMP_I2C_LEN_FIELD], nb->data, pkt_len);
	tx_len = pkt_len;
	tx_pos = 0U;
	tx_ready = true;

	k_spin_unlock(&tx_lock, key);

	/* DRDY goes high only after tx_ready is set. */
	drdy_set(1);

	LOG_DBG("buffered %u byte response", pkt_len);
	smp_packet_free(nb);

	return 0;
}

static uint16_t smp_i2c_get_mtu(const struct net_buf *nb)
{
	/* One peer per I2C bus, so a single fixed MTU applies. */
	ARG_UNUSED(nb);
	return CONFIG_MCUMGR_TRANSPORT_I2C_MTU;
}

/* Transport initialisation */

static void smp_i2c_start(void)
{
	int rc = 0;

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C device not ready");
		return;
	}

#if SMP_I2C_HAS_DRDY
	if (!gpio_is_ready_dt(&drdy_gpio)) {
		LOG_ERR("data-ready GPIO not ready");
		return;
	}

	rc = gpio_pin_configure_dt(&drdy_gpio, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		LOG_ERR("data-ready GPIO configure failed: %d", rc);
		return;
	}
#endif

	smp_i2c_transport.functions.output = smp_i2c_tx_pkt;
	/* get_mtu is deprecated and not called by the SMP layer, but still set
	 * for transports that read it; silence the deprecation on assignment.
	 */
	TOOLCHAIN_DISABLE_WARNING(TOOLCHAIN_WARNING_DEPRECATED_DECLARATIONS);
	smp_i2c_transport.functions.get_mtu = smp_i2c_get_mtu;
	TOOLCHAIN_ENABLE_WARNING(TOOLCHAIN_WARNING_DEPRECATED_DECLARATIONS);

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

#ifdef CONFIG_ZTEST
/* Return the transport to its power-on state between unit tests. */
void smp_i2c_test_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	(void)tx_discard_locked();
	k_spin_unlock(&tx_lock, key);

	if (rx_nb != NULL) {
		smp_packet_free(rx_nb);
		rx_nb = NULL;
	}
	rx_state = SMP_I2C_RX_LEN_HIGH;
	rx_expected = 0U;
	rx_received = 0U;
}
#endif

MCUMGR_HANDLER_DEFINE(smp_i2c, smp_i2c_start);
