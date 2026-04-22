/*
* File: main.c
*
* Description: Sample application to test all type of observer and how flow work between channel
*  		and observation. Controlling method of the Channel and observer.
*  		HLP protocol test and HOP calculation how is it working.!
*  		zbus Observer Types + HLP (Priority Boost) with real BME280 sensor.
*
* --> Listener         --> synchronous callback in publisher context
* --> Async Listener   --> deferred callback in work queue (net buf for the poll message to notify)
* --> Msg Subscriber   --> full message copy via FIFO
* --> Subscriber       --> channel reference only, must read()
* --> HLP Priority Boost --> publisher boosted to highest observer priorty
*
* Copyright (c) 2026 Aerlync Lab
* SPDX-License-Identifier: Apache-2.0
*
*/

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#define THREAD_STACK_SIZE   1024
#define PUBLISHER_PRI 5
#define MS_PRI        3
#define S_PRI         4
#define S_THREAD_PRI  6
#define AS_PRI        -1

LOG_MODULE_DECLARE(zbus, CONFIG_ZBUS_LOG_LEVEL);
/*
 * @brief message which publisher will publish into the channel
 *
 * @param timestamp_ms time@publish
 * @param sequence tracking the sequence
 * @param temp/hum two variable to transfer to test.!
 * @param valid to check data is trust.
 */
struct sensor_msg {
	int32_t tempt;
	int32_t humty;
	int32_t ph;
	uint32_t sequence;
	uint64_t timestamp_ms;
	bool valid;
};

/*
 * Comman user data act as Meta-data over the channel for the observer
 *
 * @brief Channel-level statistics accessible to all observers via
 * zbus_chan_user_data(chan).
 */
struct sensor_chan_meta {
	uint32_t total_published;
	uint32_t sensor_errors;
	uint32_t validator_rejects;
	int64_t  last_publish_ms;
};

static struct sensor_chan_meta chan_meta;
static const struct device *const bme280_dev = DEVICE_DT_GET_ANY(bosch_bme280);

/*
 * Validator function to publish the message into the channel
 *
 * @brief Validate sensor_msg before it is committed to sensor_chan
 *
 * Called by zbus_chan_pub() while the channel mutex is held, before VDED.
 * Return false to abort the publish entirely — no observer is notified and
 * the channel buffer is NOT updated.
 *
 * @return true if message is safe to publish, false to reject.
 */
static bool sensor_msg_validator(const void *msg, size_t msg_size)
{
	ARG_UNUSED(msg_size);
	const struct sensor_msg *m = msg;

	if (m->valid != true) {
		return 0;
	}

	if (m->tempt < -4000 || m->tempt > 8500) {
		LOG_WRN("VALIDATOR: temperature %d.%02d out of BME280 range!", m->tempt / 100,
				m->tempt % 100);
		return false;
	}
	if (m->humty < 0 || m->humty > 10000) {
		LOG_WRN("VALIDATOR: humidity %d.%02d out of range!", m->humty / 100,
				m->humty % 100);
		return false;
	}

	return true;
}
/*
 * @brief
 * Channel create and struct section will handle the observer list
 *
 * arguments:
 *  name       = sensor_chan
 *  msg_type   = struct sensor_msg
 *  validator  = sensor_msg_validator
 *  user_data  = &chan_meta
 *  observers  = ordered list
 *  init       = msg_init zero value.
 */
ZBUS_CHAN_DEFINE(sensor_chan, struct sensor_msg, sensor_msg_validator, &chan_meta,
		ZBUS_OBSERVERS(sensor_listener, sensor_async, sensor_msgsub, sensor_sub),
		ZBUS_MSG_INIT(.tempt = 0, .humty = 0, .sequence = 0, .timestamp_ms = 0,
			.ph = 0, .valid = false));

/*
 * listener --> synchronous callback.!
 *
 * @brief
 * Runs IN THE PUBLISHER'S THREAD during VDED while channel is locked.
 * Use zbus_chan_const_msg() — zero-copy, no memcpy.
 *
 * Direct message copy reference to the message in the channel.
 *
 * @note:
 *
 * DO NOT call zbus_chan_pub/read/claim here (deadlock — channel locked).
 * Channel is LOCKED when this runs — use zbus_chan_const_msg() only.
 */

static void sensor_listener_cb(const struct zbus_channel *chan)
{
	const struct sensor_msg *msg = zbus_chan_const_msg(chan);
	const struct sensor_chan_meta *meta = zbus_chan_user_data(chan);
	int prio_now = k_thread_priority_get(k_current_get());

	if (msg->valid != true) {
		LOG_WRN("[LISTENER] seq=%u — SENSOR ERROR (valid=false)", msg->sequence);
		return;
	}

#ifdef CONFIG_ZBUS_PRIORITY_BOOST
	/* With HLP correctly configured, prio_now should be PRIO_MSG_SUB=3 */
	bool boosted = (prio_now < MS_PRI);
	LOG_INF("[LISTENER] seq=%u | temp=%d.%02d°C | hum=%d.%02d%% | prio_VDED=%d | HLP:%s |"
			"total_pub=%u", msg->sequence, msg->tempt / 100, abs(msg->tempt % 100),
			msg->humty / 100, msg->humty % 100, prio_now,
			boosted ? "BOOSTED" : "NOT BOOSTED", meta->total_published);
#else
	LOG_INF("[LISTENER] seq=%u | temp=%d.%02d°C | hum=%d.%02d%% | prio_VDED=%d | HLP:DISABLED",
			msg->sequence, msg->tempt / 100, abs(msg->tempt % 100),
			msg->humty / 100, msg->humty % 100, prio_now);
#endif
}

ZBUS_LISTENER_DEFINE(sensor_listener, sensor_listener_cb);

/* Aysn listener -->  Work Queue Callback
 *
 * @brief
 *
 * VDED submits a work item to the system workqueue.
 * Runs AFTER VDED completes (channel is already unlocked).
 * Receives a COPY of the message as second parameter.
 * System workqueue priority = -1 (cooperative, higher than everything)
 */
static void sensor_async_cb(const struct zbus_channel *chan, const void *msg_copy)
{
	ARG_UNUSED(chan);
	const struct sensor_msg *msg = msg_copy;

	if (msg->valid != true) {
		LOG_WRN("[ASYNC] seq=%u sensor read failed — skipping", msg->sequence);
		return;
	}

	if (msg->tempt > 3500) {
		LOG_WRN("[ASYNC] HIGH TEMP ALERT: %d.%02d°C at seq=%u ts=%llu ms",
				msg->tempt / 100, msg->tempt % 100, msg->sequence,
				msg->timestamp_ms);
		return;
	}

	LOG_INF("[ASYNC] seq=%u temp=%d.%02d°C hum=%d.%02d%% | workqueue ctx", msg->sequence,
			msg->tempt / 100, abs(msg->tempt % 100), msg->humty / 100,
			msg->humty % 100);
}

ZBUS_ASYNC_LISTENER_DEFINE(sensor_async, sensor_async_cb);

/*
 * message subscriber — full message copy via net_buf FIFO
 *
 * @brief
 *
 * Priority = 3  (HIGHER than publisher at 5)
 * This is why HLP is needed: without the boost, msg_sub_thread would
 * preempt the publisher mid-VDED as soon as VDED enqueues the net_buf.
 *
 * With HLP:
 *   - publisher boosted to 3 before VDED starts
 *   - runs all VDED steps without preemption
 *   - restored to 5 after zbus_chan_pub() returns
 *   - THEN msg_sub_thread wakes and processes its copy safely
 *
 * @note:
 * Never misses messages — each publish enqueues a separate net_buf copy.
 * Receives FULL MESSAGE COPY via FIFO — never loses data.
 */

ZBUS_MSG_SUBSCRIBER_DEFINE(sensor_msgsub);

static void msg_sub_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct zbus_channel *chan;
	struct sensor_msg local_msg;

	LOG_INF("[MSG_SUB] started | prio=%d (higher than publisher=5, HLP needed)",
			k_thread_priority_get(k_current_get()));

#ifdef CONFIG_ZBUS_PRIORITY_BOOST
	zbus_obs_attach_to_thread(&sensor_msgsub);
	LOG_DBG("[MSG_SUB] attached to HLP — publisher will be boosted to prio=%d",
			MS_PRI - 1);
#endif
	while (1) {

		/*
		 * Here zbus use net_buf to copy the message into the Message subscriber.
		 * Blocks here. CPU can idle.
		 * Wakes with complete message copy in local_msg.
		 * eg:
		 * Even if publisher fires 3 times quickly, all 3 copies
		 * are queued — none are lost.
		 */

		/* wait to get notiify and messsage.!)*/
		int ret = zbus_sub_wait_msg(&sensor_msgsub, &chan, &local_msg, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("msg_sub wait error: %d", ret);
			continue;
		}

		/* check from which channel got notification*/
		if (chan != &sensor_chan) {
			continue;
		}

		if (local_msg.valid != true) {
			LOG_WRN("[MSG_SUB] seq=%u — received invalid reading", local_msg.sequence);
			continue;
		}

		LOG_INF("[MSG_SUB] seq=%u | temp=%d.%02d°C | hum=%d.%02d%% | ph=%d.%02d hPa |"
				"ts=%llu ms | full-copy, never lost", local_msg.sequence,
				local_msg.tempt / 100, abs(local_msg.tempt % 100),
				local_msg.humty / 100, local_msg.humty % 100,
				local_msg.ph / 100, local_msg.ph % 100,
				local_msg.timestamp_ms);
	}
}


K_THREAD_DEFINE(msg_sub_tid, THREAD_STACK_SIZE, msg_sub_thread_fn, NULL, NULL, NULL, MS_PRI, 0, 0);

/*
 * subscriber -->  Channel Reference Thread
 *
 * @brief.
 * Gets only the channel reference  must call zbus_chan_read() explicitly.
 * Here now one publisher.
 *
 * attach_to_thread(): tells HLP about this observer's priority so.
 * it is included in the HOP (Highest Observer Priority) calculation.
 *
 * @note: if publisher publishes twice before this thread wakes and reads,
 * it only sees the LATEST value (not both). Unlike MSG_SUBSCRIBER which
 * keeps every copy.
 */

ZBUS_SUBSCRIBER_DEFINE(sensor_sub, S_PRI);

static void sub_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct zbus_channel *chan;
	struct sensor_msg local_msg;

	LOG_INF("[SUB] started | prio=%d (lower than publisher, no preemption risk)",
			k_thread_priority_get(k_current_get()));

#ifdef CONFIG_ZBUS_PRIORITY_BOOST
	/*
	 * @note:
	 * Attach this observer to the current thread so HLP can see its priority. Without this,
	 * HLP ignores this subscriber when computing the Highest Observer Priority (HOP).
	 *
	 * Since sub_thread is prio=6 (lower than publisher prio=5), it doesn't actually
	 * change the HOP — msg_sub at prio=3 is still the highest. But attach is good practice.
	 */

	zbus_obs_attach_to_thread(&sensor_sub);
#endif

	while (1) {
		/* Blocks here. Wakes when VDED pushes channel ref to our queue. */
		int ret = zbus_sub_wait(&sensor_sub, &chan, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("[SUB] wait error: %d", ret);
			continue;
		}

		if (chan != &sensor_chan) {
			continue;
		}

		/*
		 * Use K_MSEC(100) — channel may still be locked by VDED or another
		 * thread. K_NO_WAIT risks -EAGAIN if VDED hasn't unlocked yet.
		 */
		ret = zbus_chan_read(&sensor_chan, &local_msg, K_MSEC(100));
		if (ret != 0) {
			LOG_ERR("[SUB] read timeout! ret=%d", ret);
			continue;
		}

		if (local_msg.valid != true) {
			LOG_WRN("[SUB] seq=%u latest value is invalid", local_msg.sequence);
			continue;
		}

		LOG_INF("[SUB] seq=%u | temp=%d.%02d°C | hum=%d.%02d%% | latest-value semantics |"
				"prio=6", local_msg.sequence, local_msg.tempt / 100,
				abs(local_msg.tempt % 100), local_msg.humty / 100,
				local_msg.humty % 100);
	}
}

K_THREAD_DEFINE(sub_tid, 1024, sub_thread_fn, NULL, NULL, NULL, S_THREAD_PRI, 0, 0);

/*
 * @brief Read temperature, humidity and pressure from BME280.
 *
 * 1. sensor_sample_fetch() --> triggers I²C read, stores raw ADC in driver
 * 2. sensor_channel_get() -->  applies Bosch compensation formula, returns
 * 				a struct sensor_value {int32_t val1; int32_t val2} where val2 is
 * 				the fractional part in MILLIONTHS (10^-6).
 *
 * @param[out] msg  populated on success
 * @return 0 on success, -errno on sensor failure
 */
static int bme280_read(struct sensor_msg *msg)
{
	struct sensor_value temp_val, hum_val, ph_val;
	int ret;

	ret = sensor_sample_fetch(bme280_dev);
	if (ret != 0) {
		LOG_ERR("sensor_sample_fetch() failed: %d", ret);
		msg->valid = false;
		return ret;
	}

	ret = sensor_channel_get(bme280_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp_val);
	if (ret != 0) {
		LOG_ERR("sensor_channel_get(TEMP) failed: %d", ret);
		msg->valid = false;
		return ret;
	}

	ret = sensor_channel_get(bme280_dev, SENSOR_CHAN_HUMIDITY, &hum_val);
	if (ret != 0) {
		LOG_ERR("sensor_channel_get(HUMIDITY) failed: %d", ret);
		msg->valid = false;
		return ret;
	}

	ret = sensor_channel_get(bme280_dev, SENSOR_CHAN_PRESS, &ph_val);
	if (ret != 0) {
		LOG_ERR("sensor_channel_get(PRESS) failed: %d", ret);
		msg->valid = false;
		return ret;
	}

	msg->tempt = (int32_t)(temp_val.val1  * 100 + temp_val.val2  / 10000);
	msg->humty = (int32_t)(hum_val.val1   * 100 + hum_val.val2   / 10000);
	msg->ph = (int32_t)(ph_val.val1 * 100 + ph_val.val2 / 10000);
	msg->valid = true;

	return 0;
}

/* publisher thread
 *
 * @brief Publisher thread — reads BME280 every 2 s, publishes to sensor_chan
 * zbus_chan_pub() will:
 *  1. Lock the channel mutex
 *  2. Copy msg into channel buffer
 *  3. If HLP: boost publisher to HOP (=3, from msg_sub)
 *  4. Run VDED: notify all observers in order
 *     - sensor_listener cb runs here (in our context)
 *     - sensor_async work item submitted to workqueue
 *     - sensor_msgsub net_buf copy enqueued
 *     - sensor_sub channel ref pushed to queue
 *  5. If HLP: restore publisher priority back to 5
 *  6. Unlock channel mutex
 */
static void publisher_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct sensor_msg msg = {0};
	uint32_t pub_count = 0;
	int ret, prio_before, prio_after;

	LOG_INF("[PUB] started | prio=%d | BME280 sensor: %s", PUBLISHER_PRI, bme280_dev->name);

	while (1) {
		pub_count++;
		msg.sequence     = pub_count;
		msg.timestamp_ms = k_uptime_get();

		ret = bme280_read(&msg);
		if (ret != 0) {
			LOG_WRN("[PUB] BME280 read failed (ret=%d), publishing invalid msg", ret);
			chan_meta.sensor_errors++;
		}

		LOG_INF("[PUB] seq=%u | temp=%d.%02d°C | hum=%d.%02d%% | ph=%d.%02d hPa | valid=%d",
				msg.sequence, msg.tempt / 100, abs(msg.tempt % 100), msg.humty / 100,
				msg.humty % 100, msg.ph / 100, msg.ph % 100, msg.valid);

		prio_before = k_thread_priority_get(k_current_get());
		LOG_INF("[PUBLISHER] prio BEFORE pub = %d (original priority)\n", prio_before);

		ret = zbus_chan_pub(&sensor_chan, &msg, K_FOREVER);
		if (ret == -EINVAL) {
			LOG_WRN("[PUB] validator rejected msg (seq=%u) — not published", msg.sequence);
			chan_meta.validator_rejects++;
		} else if (ret != 0) {
			LOG_ERR("[PUB] zbus_chan_pub failed: %d", ret);
		} else {
			chan_meta.total_published++;
			chan_meta.last_publish_ms = msg.timestamp_ms;
		}

		prio_after = k_thread_priority_get(k_current_get());

		LOG_INF("[PUBLISHER] prio AFTER  pub = %d (restored by HLP)\n", prio_after);

		/* Priority delta: should be 0 after HLP restores. Catch it here.
		 * @if prio_before - prio_after is 0
		 *
		 * @note: because HLP RESTORES the priority after pub. The boost was temporary,
		 * only visible DURING pub (inside the listener callback). That is why we print
		 * prio inside sensor_listener_cb above that is the only way
		 * to catch the boosted value.
		 */


		if (prio_before != prio_after) {
			LOG_ERR("[PUB] HLP RESTORE FAILED: before=%d after=%d", prio_before, prio_after);
		}

		k_sleep(K_MSEC(2000));
	}
}

K_THREAD_DEFINE(publisher_tid, 2048, publisher_thread_fn, NULL, NULL, NULL, PUBLISHER_PRI, 0, 0);

int main(void)
{
	LOG_INF("\n\t<<< zbus + BME280 on nRF52840DK >>>\n");

	if (bme280_dev == NULL) {
		LOG_ERR("No bosch,bme280 node found in devicetree!");
		return -ENODEV;
	}

	if (!device_is_ready(bme280_dev)) {
		LOG_ERR("BME280 device '%s' is NOT ready!", bme280_dev->name);
		return -ENODEV;
	}

	LOG_INF("BME280 device '%s' ready!", bme280_dev->name);
	LOG_INF("Priority map:");
	LOG_INF("  sys workqueue : %2d  (cooperative, runs async_listener)", AS_PRI);
	LOG_INF("  msg_sub thread: %2d  (higher than publisher — HLP needed)", MS_PRI);
	LOG_INF("  publisher     : %2d  (HLP boosts to 3 during VDED)", PUBLISHER_PRI);
	LOG_INF("  sub thread    : %2d  (lower than publisher, safe)", S_THREAD_PRI);
	LOG_INF("Channel: sensor_chan | validator: BME280 range check | user_data: &chan_meta");

	while (1) {
		/* Print channel statistics every 10 s from main */
		k_sleep(K_SECONDS(3));

		const struct sensor_chan_meta *meta = zbus_chan_user_data(&sensor_chan);

		LOG_INF("[MAIN] stats: published=%u errors=%u rejected=%u last_pub=%lld ms",
				meta->total_published, meta->sensor_errors, meta->validator_rejects,
				meta->last_publish_ms);
	}

	return 0;
}
