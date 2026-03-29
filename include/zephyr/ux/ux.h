/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_UX_UX_H_
#define ZEPHYR_INCLUDE_UX_UX_H_

#include <zephyr/ux/ux_event.h>
#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UX handler descriptor.
 */
struct ux_handler {
	int (*dispatch)(const struct ux_event *event);
};

/**
 * @brief Register a UX output handler at link time.
 *
 * @param _name  name for this handler entry.
 * @param _fn    dispatcher function for the event.
 */
#define UX_HANDLER_DEFINE(_name, _fn)					\
	static const STRUCT_SECTION_ITERABLE(ux_handler, _name) = {	\
		.dispatch = (_fn),					\
	}

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


#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_UX_UX_H_ */
