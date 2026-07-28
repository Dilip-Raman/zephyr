#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define THREAD_PRIORITY 5

K_MBOX_DEFINE(my_mailbox);
K_THREAD_STACK_DEFINE(thread_a_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_b_stack, STACK_SIZE);
struct k_thread thread_a_data;
struct k_thread thread_b_data;

void thread_a_entry(void *p1, void *p2, void *p3) {
    struct k_mbox_msg rx_msg;
    int counter = 0;

    printk("Entry thread A CPU %u\n", arch_proc_id());
    while (1) {
        rx_msg.size = 0;
        rx_msg.rx_source_thread = NULL;
        rx_msg.tx_target_thread = NULL;
	printk("Before thread A get mail box\n");
        k_mbox_get(&my_mailbox, &rx_msg, NULL, K_USEC(100));
	printk("After thread A get counter %d \n", counter);

        if (counter++ % 100 == 0) {
            printk("Thread A running...\n");
	    printk("Thread A CPU %u\n", arch_proc_id());
        }
    }
}

void thread_b_entry(void *p1, void *p2, void *p3) {
    struct k_mbox_msg tx_msg;
    int counter = 0;

    printk("Entry thread B CPU %u\n", arch_proc_id());
    while (1) {
        tx_msg.info = 123;
        tx_msg.size = 0;
        tx_msg.tx_data = NULL;
        tx_msg.tx_target_thread = NULL;
	 printk("Before thread B get mail box\n");

        k_mbox_put(&my_mailbox, &tx_msg, K_NO_WAIT);
	printk("After thread B put counter %d \n", counter);

        if (counter++ % 100 == 0) {
            printk("Thread B running...\n");
	    printk("Thread B CPU %u\n", arch_proc_id());
        }

	printk("Thread B before 50  usec\n");

        k_sleep(K_USEC(50));
	printk("After 50 used in thread B\n");
    }
}

int main(void) {
    printk("Starting SMP Mailbox Race Reproducer...\n");

    k_thread_create(&thread_a_data, thread_a_stack,
                    K_THREAD_STACK_SIZEOF(thread_a_stack),
                    thread_a_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&thread_b_data, thread_b_stack,
                    K_THREAD_STACK_SIZEOF(thread_b_stack),
                    thread_b_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);

    /* Main thread can just sleep forever */
    k_sleep(K_FOREVER);
    return 0;
}
