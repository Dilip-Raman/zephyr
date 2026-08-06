.. smp_mailbox_race:

SMP Mailbox Race Reproducer
############################

Overview
********

This sample reproduces the mailbox **synchronous-send dispose race** with
just two threads:

* Thread A waits for a message (``k_mbox_get()`` with a 100 us timeout).
* Thread B sends an empty message (``k_mbox_put(..., K_NO_WAIT)``) and
  sleeps 50 us.

When A is already waiting, B's put matches A, and B then pends until A
has consumed the message. On SMP, A can consume and wake B while B is
still inside its context-switch window. That wake-up is lost, and B
stays pended forever with no timeout -- the system appears frozen.

Expected behavior
*****************

On an SMP target (with or without the wait-queue walk race fix):

* Thread B's "sent" counter stops advancing -- B is stranded.
* Thread A keeps looping on its 100 us timeout.

This sample is **not** a pass/fail test: the dispose race is still open
on every current kernel. The wait-queue walk race
(zephyrproject-rtos/zephyr#111643) is covered by the ztest in
``tests/kernel/mbox/mbox_stress``.

Diagnosing the strand
*********************

Start QEMU with ``-gdb tcp::1234`` (no ``-S``) and attach:

.. code-block:: shell

   x86_64-zephyr-elf-gdb zephyr.elf
   (gdb) target remote :1234
   (gdb) p/x thread_b_data.base.pended_on
   $1 = 0x0            # stranded: no wait queue, no pending wakeup
   (gdb) p/x thread_b_data.base.timeout.node.next
   $2 = 0x0            # K_FOREVER: no timeout either

Board support
*************

SMP targets only: ``qemu_x86_64`` (2 CPUs) and
``qemu_cortex_a53/qemu_cortex_a53/smp`` reproduce it reliably.
Single-core boards cannot hit the race.
