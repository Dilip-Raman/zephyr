#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <nrfx_egu.h>

#include <hal/nrf_egu.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(time_race, LOG_LEVEL_INF);

#define TEST_ITERATIONS          10000U

#define WORK_DELAY_TICKS         K_TICKS(1)

#define EGU_RESCHEDULE_DELAY     K_MSEC(100)

#define EGU0_IRQ_PRIORITY        0U

#define EGU0_IRQn                20U

#define LED1_PIN   NRF_GPIO_PIN_MAP(0, 13)
#define LED2_PIN   NRF_GPIO_PIN_MAP(0, 14)

static struct k_work_delayable g_dwork;

static atomic_t g_iter_count;

static atomic_t g_stop;

static atomic_t g_egu_preempt_count;

static void egu0_isr_handler(void)
{
    nrf_egu_event_clear(NRF_EGU0, NRF_EGU_EVENT_TRIGGERED0);

    nrf_gpio_pin_set(LED2_PIN);

    atomic_inc(&g_egu_preempt_count);

    if (!atomic_get(&g_stop)) {
        k_work_reschedule(&g_dwork, EGU_RESCHEDULE_DELAY);
    }

    nrf_gpio_pin_clear(LED2_PIN);
}

static void work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    uint32_t count = atomic_inc(&g_iter_count) + 1;

    nrf_gpio_pin_set(LED1_PIN);

    if (count % 500 == 0) {
        LOG_INF("Iteration %u | EGU preempts so far: %u",
                count, (uint32_t)atomic_get(&g_egu_preempt_count));
    }

    if (atomic_get(&g_stop) || count >= TEST_ITERATIONS) {
        atomic_set(&g_stop, 1);
        LOG_INF("Test complete. Total iterations: %u", count);
        LOG_INF("Total EGU preempt events:        %u",
                (uint32_t)atomic_get(&g_egu_preempt_count));
        nrf_gpio_pin_clear(LED1_PIN);
        return;
    }

    nrf_egu_task_trigger(NRF_EGU0, NRF_EGU_TASK_TRIGGER0);

    k_work_schedule(dwork, WORK_DELAY_TICKS);

    nrf_gpio_pin_clear(LED1_PIN);
}


static void egu0_init(void)
{
    IRQ_CONNECT(EGU0_IRQn, EGU0_IRQ_PRIORITY, egu0_isr_handler, NULL, 0);

    nrf_egu_int_enable(NRF_EGU0, NRF_EGU_INT_TRIGGERED0);

    irq_enable(EGU0_IRQn);

    LOG_INF("EGU0 configured at NVIC priority %u (IRQ %u)",
            EGU0_IRQ_PRIORITY, EGU0_IRQn);
}


static void gpio_init(void)
{
    nrf_gpio_cfg_output(LED1_PIN);
    nrf_gpio_cfg_output(LED2_PIN);
    nrf_gpio_pin_clear(LED1_PIN);
    nrf_gpio_pin_clear(LED2_PIN);
}


int main(void)
{
    LOG_INF("Setup:");
    LOG_INF("  System tick (RTC1) NVIC priority : 1");
    LOG_INF("  EGU0 ISR           NVIC priority : %u (HIGHER = preempts RTC1)",
            EGU0_IRQ_PRIORITY);
    LOG_INF("  Work item delay                  : 1 tick (1ms)");
    LOG_INF("  Target iterations                : %u", TEST_ITERATIONS);
    LOG_INF("Expected result (UNFIXED kernel):");
    LOG_INF("  Kernel assert fires within ~100-2000 iterations.");
    LOG_INF("  Look for: ASSERTION FAIL [!sys_dnode_is_linked(&to->node)]");
    LOG_INF("Expected result (PATCHED kernel):");
    LOG_INF("  All %u iterations complete. No assert.", TEST_ITERATIONS);

    gpio_init();

    k_work_init_delayable(&g_dwork, work_handler);

    egu0_init();

    LOG_INF("Starting test...");
    k_work_schedule(&g_dwork, WORK_DELAY_TICKS);

    uint32_t last_count = 0;
    uint32_t watchdog   = 0;

    while (!atomic_get(&g_stop)) {
        k_msleep(1000);
        watchdog++;

        uint32_t current = atomic_get(&g_iter_count);

        if (current == last_count) {
            LOG_WRN("No progress detected! iter=%u (watchdog=%u)",
                    current, watchdog);
            if (watchdog >= 5) {
                LOG_ERR("DEADLOCK DETECTED — timeout list likely corrupted.");
                LOG_ERR("(CONFIG_ASSERT=n path: double-linked list corruption)");
                break;
            }
        } else {
            watchdog = 0;
            last_count = current;
        }
    }

    uint32_t final_count = atomic_get(&g_iter_count);
    uint32_t egu_count   = atomic_get(&g_egu_preempt_count);

    if (final_count >= TEST_ITERATIONS) {
        LOG_INF("RESULT: PASS — All %u iterations completed", TEST_ITERATIONS);
        LOG_INF("  EGU0 preemption events: %u", egu_count);
        LOG_INF("  (Kernel is PATCHED or race was not triggered)");
    } else {
        LOG_ERR("RESULT: FAIL — Stopped at iteration %u / %u",
                final_count, TEST_ITERATIONS);
        LOG_ERR("  EGU0 preemption events: %u", egu_count);
        LOG_ERR("  Check for assert output above.");
    }

    return 0;
}
