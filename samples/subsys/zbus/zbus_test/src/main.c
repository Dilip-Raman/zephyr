/*
 * Aerlync Labs (c) -2026
 *
 * File: main.c
 * Description: Sample application to test all type of observer and how flow work between channel
 * 		and observation. Controlling method of the Channel and observer.!
 * 		HLP protocol test and HOP calculation how is it working.!
 *
 * --> Listener         --> synchronous callback in publisher context
 * --> Async Listener   --> deferred callback in work queue (net buf for the poll message to notify)
 * --> Msg Subscriber   --> full message copy via FIFO
 * --> Subscriber       --> channel reference only, must read()
 * --> HLP Priority Boost --> publisher boosted to highest observer priorty
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#define THREAD_SIZE   1024
#define PUBLISHER_PRI 5
#define MS_PRI        3
#define S_PRI         4
#define S_THREAD_PRI  6
#define AS_PRI        -1

LOG_MODULE_REGISTER(zbus_demo, LOG_LEVEL_DBG);

/*
 * @brief message which publisher will publish into the channel
 *
 * @param timestamp_ms time@publish
 * @param sequence tracking the sequence
 * @param temp/hum two variable to transfer to test.!
 */
struct sensor_msg {
	int32_t tempt;
	int32_t humty;
	uint32_t sequence;
	uint64_t timestamp_ms;
};

/*
 * @brief
 * Channel create and struct section will handle the observer list
 *
 * required channel name, C struct need to use in the channel, TODO: currently no validater( just to
 * validate the struct data before publish into the channel, TODO: no user data ( meta-data),
 * Observer list with their priority this will be sys_list handle, initial value to the channel
 * buffer.!
 */
ZBUS_CHAN_DEFINE(sensor_chan, struct sensor_msg, NULL, NULL,
		 ZBUS_OBSERVERS(sensor_listener, sensor_async, sensor_msgsub, sensor_sub),
		 ZBUS_MSG_INIT(.tempt = 0, .humty = 0, .sequence = 0, .timestamp_ms = 0));

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
	int prio_now = k_thread_priority_get(k_current_get());

#ifdef CONFIG_ZBUS_PRIORITY_BOOST
	/* With HLP correctly configured, prio_now should be PRIO_MSG_SUB=3 */
	bool boosted = (prio_now <= 3);
	printk("\n[LISTENER] seq=%u | prio_during_VDED=%d | HLP: %s\n", msg->sequence, prio_now,
	       boosted ? "BOOSTED above 3   <-- publisher raised, msg_sub cannot preempt"
		       : "NOT BOOSTED   <-- still prio=5, msg_sub WILL preempt mid-VDED!");
#else
	printk("\n[LISTENER] seq=%u | prio_during_VDED=%d | HLP: DISABLED\n", msg->sequence,
	       prio_now);
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
	const struct sensor_msg *msg = msg_copy;

	printk("[ASYNC_LISTENER] seq=%u temp=%d.%02d°C hum=%d.%02d%% | sys-workqueue context"
	       "(prio= AS_PRI)\n",
	       msg->sequence, msg->tempt / 100, msg->tempt % 100, msg->humty / 100,
	       msg->humty % 100);
	/* TODO: Have to test */
	// k_busy_wait(500);
	/*But in sync listener don't use it*/
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
	int pri = k_thread_priority_get(k_current_get());

	printk("[MSG_SUBSCRIBER] thread started | priority=%d | I am higher-prio than "
	       "publisher(5)!\n",
	       pri);
	printk("[MSG_SUBSCRIBER] HLP must boost publisher to 3 so I don't preempt mid-VDED\n\n");

#ifdef CONFIG_ZBUS_PRIORITY_BOOST
	zbus_obs_attach_to_thread(&sensor_msgsub);
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
		if (chan == &sensor_chan) {
			printk("[MSG_SUBSCRIBER] seq=%u temp=%d.%02d°C hum=%d.%02d%% | full copy,"
			       "never misses | ts=%llu ms\n",
			       local_msg.sequence, local_msg.tempt / 100, local_msg.tempt % 100,
			       local_msg.humty / 100, local_msg.humty % 100,
			       local_msg.timestamp_ms);
		}
	}
}

K_THREAD_DEFINE(msg_sub_tid, THREAD_SIZE, msg_sub_thread_fn, NULL, NULL, NULL, MS_PRI, 0, 0);

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
	int pri = k_thread_priority_get(k_current_get());

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

	printk("[SUBSCRIBER] thread started | priority=%d| lower than publisher, no preemption"
	       "risk\n\n",
	       pri);
	while (1) {
		/*
		 * Blocks here. Wakes when channel reference pushed to queue.
		 * Channel will be locked during VDED
		 */
		int ret = zbus_sub_wait(&sensor_sub, &chan, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("sub wait error: %d", ret);
			continue;
		}

		if (chan == &sensor_chan) {
			/*
			 * Must read explicitly.
			 * Use K_MSEC(100) — channel may still be locked by VDED
			 * or other threads. Never use K_NO_WAIT here..
			 */
			ret = zbus_chan_read(&sensor_chan, &local_msg, K_MSEC(100));
			if (ret == 0) {
				printk("[SUBSCRIBER] seq=%u temp=%d.%02d°C hum=%d.%02d%%"
				       "| ref-only, must read() | prio=6\n",
				       local_msg.sequence, local_msg.tempt / 100,
				       local_msg.tempt % 100, local_msg.humty / 100,
				       local_msg.humty % 100);
			} else {
				LOG_ERR("[SUBSCRIBER] read timeout! ret=%d", ret);
			}
		}
	}
}

K_THREAD_DEFINE(sub_tid, 1024, sub_thread_fn, NULL, NULL, NULL, S_THREAD_PRI, 0, 0);

/* publisher thread
 *
 * @brief Main VDED flow happens.!
 *
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

	struct sensor_msg msg = {
		.tempt = 2000,
		.humty = 5500,
		.sequence = 0,
		.timestamp_ms = 0,
	};
	uint32_t pub_count = 0;
	int ret, prio_before, prio_after;

	printk("[PUBLISHER] thread started | priority=5\n");
	printk("[PUBLISHER] msg_sub thread is priority=3 (higher than me!)\n");
#ifdef CONFIG_ZBUS_PRIORITY_BOOST
	printk("[PUBLISHER] HLP ENABLED: I will be boosted to 3 during VDED so msg_sub cannot"
	       "preempt mid-publish\n\n");
#else
	printk("[PUBLISHER] HLP DISABLED: msg_sub (prio=3) CAN preempt me mid-VDED. Watch for"
	       "races.\n\n");
#endif

	while (1) {
		pub_count++;
		msg.sequence = pub_count;
		msg.timestamp_ms = k_uptime_get();
		msg.tempt = 2000 + (pub_count * 50);
		msg.humty = 5500 + (pub_count * 20);

		if (msg.tempt > 4000) {
			msg.tempt = 2000;
		}
		if (msg.humty > 9500) {
			msg.humty = 5500;
		}

		printk("\n<<<<<<<<<Publish count>>>>>>>>>>>> %u\n", pub_count);
		printk("[PUBLISHER] temp=%d.%02d°C hum=%d.%02d%% seq=%u\n", msg.tempt / 100,
		       msg.tempt % 100, msg.humty / 100, msg.humty % 100, msg.sequence);

		/* Capture priority BEFORE publish */
		prio_before = k_thread_priority_get(k_current_get());
		printk("[PUBLISHER] prio BEFORE pub = %d (my natural priority)\n", prio_before);

		ret = zbus_chan_pub(&sensor_chan, &msg, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("Publish failed: %d", ret);
		}

		/* Capture priority AFTER publish — should be restored to 5 */
		prio_after = k_thread_priority_get(k_current_get());
		printk("[PUBLISHER] prio AFTER  pub = %d (restored by HLP)\n", prio_after);
		printk("[PUBLISHER] Delta (boost depth) = %d %s\n\n", prio_before - prio_after,
		       (prio_before == prio_after) ? "(restored correctly)"
						   : "(UNEXPECTED: not restored!)");
		/*
		 * @if prio_before - prio_after is 0
		 *
		 * @note: because HLP RESTORES the priority after pub. The boost was temporary,
		 * only visible DURING pub (inside the listener callback). That is why we print
		 * prio inside sensor_listener_cb above that is the only way
		 * to catch the boosted value.
		 */

		/* every 2s publish*/
		k_sleep(K_MSEC(2000));
	}
}

K_THREAD_DEFINE(publisher_tid, 2048, publisher_thread_fn, NULL, NULL, NULL, PUBLISHER_PRI, 0, 0);

int main(void)
{
	printk("\n\t<<<<<<<<< Zephyr zbus & HLP>>>>>>>>>\n\n");
	printk("Priority map:\n");
	printk("  sys workqueue : %d  (highest, runs async_listener)\n", AS_PRI);
	printk("  msg_sub thread:  %d  (higher than publisher!)\n", MS_PRI);
	printk("  publisher     :  %d (HLP boosts this to 3 during VDED)\n", PUBLISHER_PRI);
	printk("  sub thread    :  %d  (lower than publisher, no risk)\n\n", S_THREAD_PRI);
	printk("Watch [LISTENER] prio_during_VDED:\n");
	printk("  HLP ON  ---> prio_during_VDED < 3  (boosted)\n");
	printk("  HLP OFF ---> prio_during_VDED = 5  (not boosted)\n\n");

	/* Give all threads time to start and attach */
	// k_sleep(K_MSEC(300));

	while (1) {
		printk("[MAIN] context exit\n");
		k_sleep(K_SECONDS(2));
		printk("[MAIN] context enter\n");
	}

	return 0;
}
