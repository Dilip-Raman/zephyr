/*
 * SPDX-FileCopyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/tc_util.h>
#include <zephyr/ztest.h>


K_SEM_DEFINE(ramp_sem, 0, 1);
static uint32_t start_cycle;
static uint32_t end_cycle;

static void tm_fn(struct k_timer *tm)
{
	end_cycle = k_cycle_get_32();
	k_sem_give(&ramp_sem);
}

/**
 * @brief Test timers can be scheduled in a ramp
 *
 * Schedules timers in a ramp of delays to verify that within a reasonable range of
 * timeouts are schedulable correctly.
 *
 * It could be that some timer drivers might have difficulty in scheduling some
 * time periods but not others due to implementation details. This test should catch those.
 *
 * The ramp is logrithmic up to what amounts to approximately 10 seconds of ticks.
 */
ZTEST(timer_ramp, test_timer_ramp)
{
	TC_PRINT("=== DEBUG INFO ===\n");
	TC_PRINT("sys_clock_hw_cycles_per_sec() = %u\n", sys_clock_hw_cycles_per_sec());
	TC_PRINT("CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC = %u\n", CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
	TC_PRINT("CONFIG_SYS_CLOCK_TICKS_PER_SEC = %u\n", CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	TC_PRINT("Cycles per tick = %u / %u = %.6f\n",
			sys_clock_hw_cycles_per_sec(),
			CONFIG_SYS_CLOCK_TICKS_PER_SEC,
			(double)sys_clock_hw_cycles_per_sec() / CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	TC_PRINT("==================\n");

	bool failed = false;
	struct k_timer tm;
	uint32_t delay = 1;

	k_timer_init(&tm, tm_fn, NULL);

	while ((delay/CONFIG_SYS_CLOCK_TICKS_PER_SEC) < 10) {
		start_cycle = k_cycle_get_32();
		k_timer_start(&tm, K_TICKS(delay), K_NO_WAIT);
		k_sem_take(&ramp_sem, K_FOREVER);

		uint32_t delta_cycles = end_cycle - start_cycle;
		uint32_t delta_ticks = k_cyc_to_ticks_floor32(delta_cycles);
		double expected_cycles = (double)delay * sys_clock_hw_cycles_per_sec() / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
		double tick_precision_loss = delta_cycles - (delta_ticks * sys_clock_hw_cycles_per_sec() / CONFIG_SYS_CLOCK_TICKS_PER_SEC);

		TC_PRINT("delay=%d ticks, delta=%d ticks (%d cycles), expected=%.2f cycles, loss=%.2f cycles\n",
				delay, delta_ticks, delta_cycles, expected_cycles, tick_precision_loss);


		if (delta_ticks > (delay + 1) || delta_ticks < delay) {
			TC_PRINT("failed: delay of %d ticks , actual %d (%d cycles)\n", delay,
					delta_ticks, delta_cycles);
			failed = true;
		} else {
			TC_PRINT("passed: delay of %d ticks, actual %d (%d cycles)\n", delay,
					delta_ticks, delta_cycles);
		}
		delay = delay*2;
	}

	zassert(failed != true, "Failed ramp test");
}

ZTEST_SUITE(timer_ramp, NULL, NULL, NULL, NULL, NULL);
