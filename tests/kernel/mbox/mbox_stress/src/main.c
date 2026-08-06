/* Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SMP mailbox stress test.
 *
 * On SMP, k_mbox_put() and k_mbox_get() walk their wait queues while a
 * timeout expiring on another CPU can detach a node mid-walk and
 * corrupt the list (zephyrproject-rtos/zephyr#111643).
 *
 * The test stresses that path with sender and receiver threads that use
 * short timeouts, then asserts that nothing was corrupted, nothing
 * returned an unexpected error, and the kernel never hung.
 *
 * The MAILBOX_STRESS_* Kconfig options control the details; see the
 * Kconfig help for what each one does.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>
#include <string.h>

#define MAX_THREADS 8

K_MBOX_DEFINE(stress_mbox);

struct sender_ctx {
	struct k_thread tcb;
	struct k_sem done_sem;
	struct k_mbox_msg tx_msg;
	uint8_t tx_data[CONFIG_MAILBOX_STRESS_MSG_SIZE];
	k_tid_t target;
	unsigned int id;
	unsigned int sent;
	unsigned int timeouts;
	unsigned int unexpected;
};

struct receiver_ctx {
	struct k_thread tcb;
	struct k_mbox_msg rx_msg;
	uint8_t rx_data[CONFIG_MAILBOX_STRESS_MSG_SIZE];
	unsigned int id;
	unsigned int got;
	unsigned int timeouts;
	unsigned int bad;
	unsigned int unexpected;
};

K_KERNEL_STACK_ARRAY_DEFINE(sender_stacks, CONFIG_MAILBOX_STRESS_N_SENDERS,
			    CONFIG_MAILBOX_STRESS_STACK_SIZE);
K_KERNEL_STACK_ARRAY_DEFINE(receiver_stacks, CONFIG_MAILBOX_STRESS_N_RECVERS +
			    CONFIG_MAILBOX_STRESS_N_BYSTANDERS,
			    CONFIG_MAILBOX_STRESS_STACK_SIZE);
K_KERNEL_STACK_DEFINE(watchdog_stack, 1024);

static struct sender_ctx senders[MAX_THREADS];
static struct receiver_ctx receivers[MAX_THREADS];
static struct k_thread watchdog_tcb;
static volatile bool hang_detected;
static volatile bool all_done;

/*
 * Set once the senders and the matched receivers have finished. The
 * bystanders never match a message, so every one of their iterations
 * costs a full tick timeout; without this flag they would keep timing
 * out for hours. Once set, they finish their current get and exit.
 */
static volatile bool stop_bystanders;

static void receiver_thread(void *p1, void *p2, void *p3)
{
	struct receiver_ctx *r = p1;
	unsigned int seq = 0;
	k_timeout_t to = CONFIG_MAILBOX_STRESS_RECV_TIMEOUT_US > 0 ?
			K_USEC(CONFIG_MAILBOX_STRESS_RECV_TIMEOUT_US) : K_NO_WAIT;

	while (1) {
		memset(&r->rx_msg, 0, sizeof(r->rx_msg));
		r->rx_msg.rx_source_thread = K_ANY;
		r->rx_msg.size = CONFIG_MAILBOX_STRESS_MSG_SIZE;

		int ret = k_mbox_get(&stress_mbox, &r->rx_msg,
				     CONFIG_MAILBOX_STRESS_MSG_SIZE > 0 ?
				     r->rx_data : NULL,
				     to);

		if (ret == 0) {
			r->got++;
			if (IS_ENABLED(CONFIG_MAILBOX_STRESS_VERIFY) &&
			    CONFIG_MAILBOX_STRESS_MSG_SIZE > 0) {
				unsigned int i;

				for (i = 0; i < r->rx_msg.size; i++) {
					if (r->rx_data[i] != (uint8_t)r->rx_msg.info) {
						r->bad++;
						printk("R%u DATA MISMATCH at byte %u "
						       "(info 0x%x got 0x%x)\n",
						       r->id, i, r->rx_msg.info,
						       r->rx_data[i]);
						break;
					}
				}
			}
		} else if (ret == -EAGAIN) {
			r->timeouts++;
		} else {
			r->unexpected++;
			printk("R%u unexpected ret %d\n", r->id, ret);
		}

		if ((++seq % 10000) == 0) {
			printk("R%u got=%u t/o=%u bad=%u\n", r->id, r->got,
			       r->timeouts, r->bad);
		}

		if (seq >= CONFIG_MAILBOX_STRESS_ITERS || stop_bystanders) {
			printk("R%u finished\n", r->id);
			break;
		}
	}
}

static void sender_thread(void *p1, void *p2, void *p3)
{
	struct sender_ctx *s = p1;
	unsigned int seq = 0;
	k_timeout_t to = CONFIG_MAILBOX_STRESS_SEND_TIMEOUT_MS > 0 ?
			K_MSEC(CONFIG_MAILBOX_STRESS_SEND_TIMEOUT_MS) : K_FOREVER;

	while (1) {
		memset(&s->tx_msg, 0, sizeof(s->tx_msg));
		s->tx_msg.tx_target_thread = s->target;
		s->tx_msg.info = s->id;
		if (CONFIG_MAILBOX_STRESS_MSG_SIZE > 0) {
			s->tx_msg.size = CONFIG_MAILBOX_STRESS_MSG_SIZE;
			memset(s->tx_data, s->id, CONFIG_MAILBOX_STRESS_MSG_SIZE);
			s->tx_msg.tx_data = s->tx_data;
		}

		if (IS_ENABLED(CONFIG_MAILBOX_STRESS_ASYNC)) {
			k_mbox_async_put(&stress_mbox, &s->tx_msg, &s->done_sem);
			k_sem_take(&s->done_sem, K_FOREVER);
			s->sent++;
		} else {
			int ret = k_mbox_put(&stress_mbox, &s->tx_msg, to);

			if (ret == 0) {
				s->sent++;
			} else if (ret == -EAGAIN) {
				s->timeouts++;
			} else {
				s->unexpected++;
				printk("S%u unexpected ret %d\n", s->id, ret);
			}
		}

		if ((++seq % 10000) == 0) {
			printk("S%u sent=%u t/o=%u\n", s->id, s->sent,
			       s->timeouts);
		}

		if (seq >= CONFIG_MAILBOX_STRESS_ITERS) {
			printk("S%u finished\n", s->id);
			break;
		}
	}
}

static void watchdog_thread(void *p1, void *p2, void *p3)
{
	unsigned int prev = 0;
	unsigned int stale = 0;

	while (!all_done) {
		unsigned int total = 0;
		int i;

		k_sleep(K_SECONDS(CONFIG_MAILBOX_STRESS_WATCHDOG_SEC));

		for (i = 0; i < CONFIG_MAILBOX_STRESS_N_SENDERS; i++) {
			total += senders[i].sent;
		}
		for (i = 0; i < CONFIG_MAILBOX_STRESS_N_RECVERS +
			     CONFIG_MAILBOX_STRESS_N_BYSTANDERS; i++) {
			total += receivers[i].got + receivers[i].timeouts;
		}

		if (total == prev) {
			stale++;
			if (stale >= 2) {
				hang_detected = true;
				printk("WATCHDOG: MAILBOX STRESS HANG - no progress "
				       "for %u s\n",
				       CONFIG_MAILBOX_STRESS_WATCHDOG_SEC * stale);
				for (i = 0; i < CONFIG_MAILBOX_STRESS_N_SENDERS; i++) {
					printk("  S%u sent=%u t/o=%u\n",
					       senders[i].id, senders[i].sent,
					       senders[i].timeouts);
				}
				for (i = 0; i < CONFIG_MAILBOX_STRESS_N_RECVERS +
					     CONFIG_MAILBOX_STRESS_N_BYSTANDERS; i++) {
					printk("  R%u got=%u t/o=%u bad=%u\n",
					       receivers[i].id, receivers[i].got,
					       receivers[i].timeouts, receivers[i].bad);
				}
			}
		} else {
			stale = 0;
			printk("WATCHDOG: total progress=%u (+%u/%u s)\n",
			       total, total - prev,
			       CONFIG_MAILBOX_STRESS_WATCHDOG_SEC);
		}
		prev = total;
	}
}

static void run_stress(void)
{
	int i;

	printk("mailbox_stress: %d senders, %d receivers, %d bystanders, "
	       "async=%d, targeted=%d, recv_t/o=%dus, msg_size=%d, verify=%d, "
	       "iters=%d\n",
	       CONFIG_MAILBOX_STRESS_N_SENDERS,
	       CONFIG_MAILBOX_STRESS_N_RECVERS,
	       CONFIG_MAILBOX_STRESS_N_BYSTANDERS,
	       IS_ENABLED(CONFIG_MAILBOX_STRESS_ASYNC),
	       IS_ENABLED(CONFIG_MAILBOX_STRESS_TARGETED),
	       CONFIG_MAILBOX_STRESS_RECV_TIMEOUT_US,
	       CONFIG_MAILBOX_STRESS_MSG_SIZE,
	       IS_ENABLED(CONFIG_MAILBOX_STRESS_VERIFY),
	       CONFIG_MAILBOX_STRESS_ITERS);

	for (i = 0; i < CONFIG_MAILBOX_STRESS_N_RECVERS +
		     CONFIG_MAILBOX_STRESS_N_BYSTANDERS; i++) {
		receivers[i].id = i;
		k_thread_create(&receivers[i].tcb, receiver_stacks[i],
				CONFIG_MAILBOX_STRESS_STACK_SIZE,
				receiver_thread, &receivers[i], NULL, NULL,
				5, 0, K_NO_WAIT);
	}

	for (i = 0; i < CONFIG_MAILBOX_STRESS_N_SENDERS; i++) {
		senders[i].id = i;
		k_sem_init(&senders[i].done_sem, 0, 1);
		senders[i].target = IS_ENABLED(CONFIG_MAILBOX_STRESS_TARGETED) ?
			&receivers[i % CONFIG_MAILBOX_STRESS_N_RECVERS].tcb : K_ANY;
		k_thread_create(&senders[i].tcb, sender_stacks[i],
				CONFIG_MAILBOX_STRESS_STACK_SIZE,
				sender_thread, &senders[i], NULL, NULL,
				5, 0, K_NO_WAIT);
	}

	k_thread_create(&watchdog_tcb, watchdog_stack, 1024,
			watchdog_thread, NULL, NULL, NULL,
			1, 0, K_NO_WAIT);

	/* Wait for the senders and the matched receivers to finish. If the
	 * kernel wedges (the bug), this never returns and the ztest timeout
	 * turns the case red.
	 */
	for (i = 0; i < CONFIG_MAILBOX_STRESS_N_SENDERS; i++) {
		k_thread_join(&senders[i].tcb, K_FOREVER);
	}
	for (i = 0; i < CONFIG_MAILBOX_STRESS_N_RECVERS; i++) {
		k_thread_join(&receivers[i].tcb, K_FOREVER);
	}

	/* Release the bystanders; they exit after their current get. */
	stop_bystanders = true;

	for (i = CONFIG_MAILBOX_STRESS_N_RECVERS; i < CONFIG_MAILBOX_STRESS_N_RECVERS +
		     CONFIG_MAILBOX_STRESS_N_BYSTANDERS; i++) {
		k_thread_join(&receivers[i].tcb, K_FOREVER);
	}

	all_done = true;
	k_thread_join(&watchdog_tcb, K_FOREVER);
}

ZTEST(mbox_stress, test_smp_wait_queue_race)
{
	unsigned int total_bad = 0;
	unsigned int total_unexpected = 0;
	unsigned int total_progress = 0;
	int i;

	run_stress();

	for (i = 0; i < CONFIG_MAILBOX_STRESS_N_RECVERS +
		     CONFIG_MAILBOX_STRESS_N_BYSTANDERS; i++) {
		total_bad += receivers[i].bad;
		total_unexpected += receivers[i].unexpected;
		total_progress += receivers[i].got;
	}
	for (i = 0; i < CONFIG_MAILBOX_STRESS_N_SENDERS; i++) {
		total_unexpected += senders[i].unexpected;
		total_progress += senders[i].sent;
	}

	printk("mailbox_stress summary: progress=%u bad=%u unexpected=%u "
	       "hang=%d\n", total_progress, total_bad, total_unexpected,
	       hang_detected);

	zassert_false(hang_detected, "watchdog declared a hang");
	zassert_equal(total_bad, 0, "payload corruption detected (%u bytes)",
		      total_bad);
	zassert_equal(total_unexpected, 0,
		      "unexpected API return values (%u)", total_unexpected);
	zassert_true(total_progress > 0, "no mailbox traffic at all");
}

ZTEST_SUITE(mbox_stress, NULL, NULL, NULL, NULL, NULL);
