/* Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Reproducer for the mailbox synchronous-send dispose race.
 *
 * Thread A waits for a message with k_mbox_get() and a 100 us timeout.
 * Thread B sends empty messages with k_mbox_put(..., K_NO_WAIT) and
 * sleeps 50 us in between.
 *
 * When A is already waiting, B's put matches A, and B then pends until
 * A has consumed the message. On SMP, A can consume and wake B while B
 * is still inside its context-switch window; that wake-up is lost, and
 * B stays pended forever with no timeout.
 *
 * Expected result: B's "sent" counter stops advancing (B is stranded),
 * while A keeps looping on its timeout. This is a diagnostic sample,
 * not a pass/fail test: the dispose race is still open on every current
 * kernel. The wait-queue walk race (#111643) is covered by the ztest in
 * tests/kernel/mbox/mbox_stress.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define THREAD_PRIORITY 5

K_MBOX_DEFINE(my_mailbox);
K_THREAD_STACK_DEFINE(thread_a_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_b_stack, STACK_SIZE);
struct k_thread thread_a_data;
struct k_thread thread_b_data;

void thread_a_entry(void *p1, void *p2, void *p3)
{
	struct k_mbox_msg rx_msg;
	int counter = 0;

	printk("Thread A entered on CPU %u\n", arch_proc_id());
	while (1) {
		rx_msg.size = 0;
		rx_msg.rx_source_thread = NULL;
		rx_msg.tx_target_thread = NULL;
		printk("Thread A: waiting for a message (100 us timeout)\n");
		k_mbox_get(&my_mailbox, &rx_msg, NULL, K_USEC(100));
		printk("Thread A: receive attempt %d done\n", counter);

		if (counter++ % 100 == 0) {
			printk("Thread A still running (CPU %u)\n", arch_proc_id());
		}
	}
}

void thread_b_entry(void *p1, void *p2, void *p3)
{
	struct k_mbox_msg tx_msg;
	int counter = 0;

	printk("Thread B entered on CPU %u\n", arch_proc_id());
	while (1) {
		tx_msg.info = 123;
		tx_msg.size = 0;
		tx_msg.tx_data = NULL;
		tx_msg.tx_target_thread = NULL;
		printk("Thread B: sending a message\n");

		k_mbox_put(&my_mailbox, &tx_msg, K_NO_WAIT);
		printk("Thread B: sent message %d\n", counter);

		if (counter++ % 100 == 0) {
			printk("Thread B still running (CPU %u)\n", arch_proc_id());
		}

		printk("Thread B: sleeping 50 us\n");

		k_sleep(K_USEC(50));
		printk("Thread B: woke up\n");
	}
}

int main(void)
{
	printk("Starting SMP Mailbox Race Reproducer\n");

	k_thread_create(&thread_a_data, thread_a_stack,
			K_THREAD_STACK_SIZEOF(thread_a_stack),
			thread_a_entry, NULL, NULL, NULL,
			THREAD_PRIORITY, 0, K_NO_WAIT);

	k_thread_create(&thread_b_data, thread_b_stack,
			K_THREAD_STACK_SIZEOF(thread_b_stack),
			thread_b_entry, NULL, NULL, NULL,
			THREAD_PRIORITY, 0, K_NO_WAIT);

	/* Nothing left to do here; the threads run forever. */
	k_sleep(K_FOREVER);
	return 0;
}
