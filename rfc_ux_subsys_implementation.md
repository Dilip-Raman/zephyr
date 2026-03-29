# RFC: subsys/ux:  — Priority-Based UX Notification Subsystem

# Abstract

Real-world Zephyr-based products (IoT sensors, medical devices, consumer electronics, automotive dashboards) require a policy layer between raw LED drivers and application code. Every product currently implements its own
ad-hoc priority state machine more lines of duplicated, inconsistently correct code per project.

This RFC proposes `subsys/ux`, a new Zephyr subsystem that provides a generic, priority-based UX notification framework. It bridges the gap between application-level system state and hardware output modalities
(LED, haptic, display, etc..) without coupling application code to hardware topology.


## Problem Description

Zephyr provides `drivers/led` and `drivers/led_strip` for raw LED control. These are correct, minimal driver implementations. They have no concept of:

- Which indication should currently be shown when two modules both want to show something simultaneously
- What to do when a higher-priority system state clears and the previous indication should automatically resume
- How to drive the same semantic event (e.g. "Battery Low") across multiple output modalities (LED + haptic) with a single call

Real-world products (IoT devices, automotive dashboards, consumer electronics) require richer feedback mechanisms:

**1. Code duplication.** Every Zephyr product re-invents the same priority state machine. No standard exists. Quality varies widely.

**2. Hardware coupling.** Application code must know whether hardware is a monochrome GPIO LED, a PWM RGB LED, or a WS2812 strip. This couples business logic to hardware topology, making board porting unnecessarily
expensive.

**3. Priority Management.** Critical system states (e.g., "Battery Low", "System Error") must override lower-priority indications (e.g., "Bluetooth Connected", "Idle Breathing") without the application manually saving/restoring  the previous state.

### Why This Cannot Be Solved at the Driver Level.?

Drivers implement mechanism. Subsystems implement policy. Adding pattern sequencing, priority queuing, or state management to `drivers/led` would violate the Zephyr driver model and would not be accepted upstream. The
correct solution is a subsystem that owns the policy and delegates output to the existing driver infrastructure.

## Proposed Change

Introduce `subsys/ux`, providing:

1. A central dispatcher thread fed by a `k_fifo` (ISR-safe, non-blocking for the caller). The dispatcher maintains a sorted priority queue and routes the highest active event to all registered output handlers.

2. Handler self-registration at link time via `UX_HANDLER_DEFINE()` using `STRUCT_SECTION_ITERABLE` — zero RAM overhead, no runtime registration function, no boot ordering dependency.

3. An LED indication handler (`ux_led_handler.c`) that executes LED patterns via a state machine driven by `k_work_delayable`. Color interpolation and led comman patterns are handled. A lightweight standardized API to "request" an indication at a specific priority level. The subsystem ensures the highest priority active indication is shown.

4. Two public functions only: `ux_notify()` and `ux_clear()`. Application code never holds `struct device *` handles for UX output and never calls hardware handlers directly.

### Architecture
High-level view:

```
[Application / Services (Bluetooth, Power, Error)]
                |
                | --> ux_notify(&event) — copies event, returns immediately
                | --> ux_clear(priority)  — removes priority, returns immediately
                | --> k_fifo to pass ux payload
                v
[UX Subsys/ (ux_notifier.c) Dispatcher thread ]
                | -->(k_fifo_get blocks here — zero CPU when idle)]
                | --> priority queue update
                |
                --> [STRUCT_SECTION_FOREACH(ux_handler)]
                   |
                   |
                   |
                   --> ux_led_handler.c   (self-registered via UX_HANDLER_DEFINE)
                   |    k_work_delayable drives pattern.
                   |    LINEAR, BREATHING, IMMEDIATE TRANSITION.
                   |    Common pattern for stepping the Error effect.
                   │    GPIO/PWM backend via extern ux_led_backend
                   |
                   |
                   --> ux_haptic_hanlder.c (Future design -no change to core notifier.)

```

### API proposal

```c

/**
 * @brief Post a UX event.
 *
 * Inserts the event into an internal FIFO and returns immediately.
 * The dispatcher thread wakes up it, updates the priority queue, and calls
 * all registered handlers with the new highest-priority active event.
 *
 * @param event  Pointer to the event to post. Must not be NULL.
 *
 * @retval 0       Queued successfully.
 * @retval -EINVAL event is NULL.
 * @retval -ENOMEM FIFO item pool exhausted.
 */
int ux_notify(const struct ux_event *event);

/**
 * @brief Clear a priority level.
 *
 * Removes the entry at priority from the priority queue.
 * If it was the highest active entry, the next highest is dispatched
 * to all handlers. If the queue becomes empty, handlers receive NULL.
 *
 * @param priority  Priority level to remove.
 */
void ux_clear(uint8_t priority);

/**
 * @brief Register a UX output handler at link time.
 *
 * @param _name  name for this handler entry.
 * @param _fn    dispatcher function for the event.
 */
#define UX_HANDLER_DEFINE(_name, _fn)                                   \
    static const STRUCT_SECTION_ITERABLE(ux_handler, _name) = {        \
        .dispatch = (_fn),                                              \
    }
```

## Event structure

```c
/**
 * @brief UX event structure.
 *
 * Holds the different payload of the module to para's the structure
 */
struct ux_event {
    uint8_t priority;
    union {
        struct ux_led_payload led;
        /* struct ux_haptic_payload haptic; — future */
    };
};

/** @brief RGB colour representation. */
struct ux_led_rgb {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
};

/** @brief Transition type for a pattern step. */
enum ux_led_transition {
        UX_LED_TRANSITION_IMMEDIATE = 0,
        UX_LED_TRANSITION_LINEAR,
        UX_LED_TRANSITION_BREATHING,
};

/**
 * @brief One step in an LED pattern.
 */
struct ux_led_step {
        struct ux_led_rgb       color;
        uint32_t        duration_ms;
        enum ux_led_transition type;
};

/**
 * @brief Complete LED indication pattern.
 */
struct ux_led_pattern {
        const struct ux_led_step *steps;
        size_t step_cnt;
        bool loop;
};

/**
 * @brief Payload for LED events, carried in struct ux_event.led.
 */
struct ux_led_payload {
    const struct ux_led_pattern *pattern;
};
```

For example:

Priority convention (application-defined, not enforced by subsystem):

| Value | Suggested use                       |
|-------|-------------------------------------|
| 0     | Idle / background animation         |
| 10    | Informational (connectivity status) |
| 20    | Activity (OTA, data sync)           |
| 30    | Warning (low battery)               |
| 40    | Critical (system error)             |

## Application usage

```c
#include <zephyr/ux/ux.h>

static const struct ux_led_step err_steps[] = {
    {
        .color = {255, 0, 0},
        .duration_ms = 200,
        type = UX_LED_TRANSITION_IMMEDIATE
    },
    {
        .color = {0, 0, 0},
        .duration_ms = 200,
        .type = UX_LED_TRANSITION_IMMEDIATE
    },
};

static const struct ux_led_pattern err_pattern = {
    .steps = err_steps,
    .step_cnt = 2,
    .loop = true,
};

void on_system_error(void)
{
    const struct ux_event ev = {
        .priority = 40,
        .led      = { .pattern = &err_pattern },
    };
    ux_notify(&ev);
}

void on_error_resolved(void)
{
    ux_clear(40);
}
```

### Configuration
- `CONIFG_UX` Enable UX subsystem
- `CONFIG_UX_MAX_PRIORITIES` Maximum concurrent priority levels.
- `CONFIH_ UX_FIFO_POOL_SIZE` FIFO item pool size
- `CONFIG_UX_THREAD_STACK_SIZE` Dispatcher thread stack size

## LED config
- `CONIFG_UX_LED` LED indication handler
- `CONFIG_UX_LED_UPDATE_PERIOD_MS` Interpolation tick period (ms)

### Dependencies
- `CONFIG_LED_STRIP` and `CONFIG_LED` required for driver.
- `CONFIG_GPIO` required for `CONFIG_UX_LED_BACKEND_GPIO`
- `CONFIG_PWM` required for `CONFIG_UX_LED_BACKEND_PWM` (follow-up)
-  Hooks into `subsys/CMakeLists.txt` and `subsys/Kconfig`
-  No new external dependencies.

## Alternatives
- **Extend `drivers/led`.** Rejected. Adding policy (priority, scheduling, state management) to drivers violates the Zephyr driver model. Adding complex pattern logic to `drivers/led` violates the separation of "driver" (mechanism) and "subsystem" (policy). Drivers should remain dumb.
- **Status quo**: Continue letting users write custom state machines.

## Testing
- Unit tests for priority logic (ensure higher priority overrides lower, and clearing restores lower).
- Integration tests with `subsys/ux` (simulated in Twister).
