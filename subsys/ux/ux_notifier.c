/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zephyr/ux/ux.h>

LOG_MODULE_REGISTER(ux_notifier, CONFIG_UX_LOG_LEVEL);

struct ux_fifo_item {
	void	*fifo_reserved;
	struct ux_event	event;
	bool	is_clear;
};

K_MEM_SLAB_DEFINE_STATIC(ux_item_slab, sizeof(struct ux_fifo_item), CONFIG_UX_FIFO_POOL_SIZE, 4);

/* extern struct k_fifo ux_fifo; */
static K_FIFO_DEFINE(ux_fifo);

struct priority_queue {
	struct ux_event event;
};

static struct priority_queue queue[CONFIG_UX_MAX_PRIORITIES];
static size_t          queue_size;

static int queue_insert(const struct ux_event *event)
{
	size_t idx;

	for (size_t i = 0; i < queue_size; i++) {
		if (queue[i].event.priority == event->priority) {
			queue[i].event = *event;
			/* in-place update, sort unchanged */
			LOG_DBG("queue: updated slot %zu (prio=%u)", i, event->priority);
			return 0;
		}
	}

	if (queue_size >= CONFIG_UX_MAX_PRIORITIES) {
		LOG_ERR("queue full (%u slots) — drop prio=%u. " "Raise CONFIG_UX_MAX_PRIORITIES.",
				CONFIG_UX_MAX_PRIORITIES, event->priority);
		return -ENOMEM;
	}

	idx = queue_size;

	for (size_t i = 0; i < queue_size; i++) {
		if (event->priority > queue[i].event.priority) {
			idx = i;
			break;
		}
	}

	if (idx < queue_size) {
		memmove(&queue[idx + 1], &queue[idx],
				(queue_size - idx) * sizeof(struct priority_queue));
	}

	queue[idx].event = *event;
	queue_size++;

	LOG_DBG("queue: inserted prio=%u at slot %zu (queue_size=%zu)",
			event->priority, idx, queue_size);

	__ASSERT(queue_size == 0 || queue[0].event.priority >=
			queue[queue_size - 1].event.priority,
			"BUG: priority queue sort order violated after insert");

	return 0;
}

static void queue_remove(uint8_t priority)
{
	for (size_t i = 0; i < queue_size; i++) {
		if (queue[i].event.priority == priority) {
			if (i < queue_size - 1) {
				memmove(&queue[i], &queue[i + 1], (queue_size - i - 1) *
					sizeof(struct priority_queue));
			}
			queue_size--;
			LOG_DBG("queue: removed prio=%u (queue_size=%zu)",
					priority, queue_size);
			return;
		}
	}
	LOG_DBG("queue: prio=%u not found — no-op", priority);
}

static void dispatch(const struct ux_event *event)
{
	int ret;

	STRUCT_SECTION_FOREACH(ux_handler, handler) {
		ret = handler->dispatch(event);

		if (ret < 0) {
			LOG_WRN("handler %p returned %d (prio=%u)", (void *)handler, ret, event ?
				event->priority : 0U);
		}
	}
}

static void ux_dispatcher_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int ret;
	struct ux_fifo_item *item;

	LOG_INF("UX dispatcher started (max_prio_slots=%d, pool=%d)",
			CONFIG_UX_MAX_PRIORITIES, CONFIG_UX_FIFO_POOL_SIZE);

	while (true) {
		item= k_fifo_get(&ux_fifo, K_FOREVER);

		if (item->is_clear) {
			queue_remove(item->event.priority);
		} else {
			ret = queue_insert(&item->event);

			if (ret < 0) {
				k_mem_slab_free(&ux_item_slab, item);
				continue;
			}
		}

		/* Return slab item immediately — it is no longer needed. */
		k_mem_slab_free(&ux_item_slab, item);

		/*
		 * Dispatch to all handlers.
		 *
		 * queue[0] is always the highest priority
		 * Handlers receive NULL when the queue is empty so they can
		 * turn off their hardware outputs cleanly.
		 */
		if (queue_size == 0U) {
			LOG_DBG("queue empty — dispatch NULL to all handlers");
			dispatch(NULL);
		} else {
			LOG_DBG("dispatching prio=%u to all handlers",queue[0].event.priority);
			dispatch(&queue[0].event);
		}
	}
}

K_THREAD_DEFINE(ux_dispatcher, CONFIG_UX_THREAD_STACK_SIZE, ux_dispatcher_thread, NULL,
		NULL, NULL, CONFIG_UX_THREAD_PRIORITY, 0, 0);


int ux_notify(const struct ux_event *event)
{
	struct ux_fifo_item *item = NULL;
	int ret;

	if (event == NULL) {
		LOG_ERR("ux_notify: NULL event pointer");
		return -EINVAL;
	}

	ret = k_mem_slab_alloc(&ux_item_slab, (void **)&item, K_NO_WAIT);

	if (ret < 0) {
		LOG_ERR("slab exhausted event prio=%u dropped Increase CONFIG_UX_FIFO_POOL_SIZE.",
				event->priority);
		return -ENOMEM;
	}

	item->fifo_reserved = NULL;
	item->event          = *event;
	item->is_clear       = false;

	k_fifo_put(&ux_fifo, item);

	return 0;
}

void ux_clear(uint8_t priority)
{
	struct ux_fifo_item *item = NULL;
	int ret;

	ret = k_mem_slab_alloc(&ux_item_slab, (void **)&item, K_NO_WAIT);

	if (ret < 0) {
		LOG_ERR("slab exhausted — clear prio=%u lost.""Increase CONFIG_UX_FIFO_POOL_SIZE.",
			priority);
		return;
	}

	item->fifo_reserved  = NULL;
	item->event.priority  = priority;
	item->is_clear        = true;

	k_fifo_put(&ux_fifo, item);
}
