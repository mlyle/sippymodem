// SPDX-License-Identifier: BSD-3-Clause

#ifndef RING_HISTORY_H
#define RING_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

/* Before filling, write is the first unused slot. Once full, fill
 * saturates while write wraps. */
static inline bool ring_history_state_is_valid(
	size_t write, size_t fill, size_t capacity)
{
	return capacity != 0 && write < capacity && fill <= capacity &&
	       (fill == capacity || write == fill);
}

/* The write slot follows the newest item; age 1 names that item. Callers
 * ensure write < capacity and 1 <= age <= capacity. */
static inline size_t ring_index_at_age(
	size_t write, size_t capacity, size_t age)
{
	size_t index = write + capacity - age;

	return index >= capacity ? index - capacity : index;
}

/* Number of newest-first items available before wrapping. An empty
 * window has no first span, even if its newest index is unspecified. */
static inline size_t ring_reverse_head_count(size_t newest, size_t count)
{
	return newest < count ? newest + 1u : count;
}

struct ring_reverse_span {
	size_t begin, end;
};

/* A newest-first window uses at most two spans. Read each span backwards
 * from its exclusive end. The caller bounds newest and count by capacity. */
static inline void ring_reverse_spans(size_t newest, size_t count,
	size_t capacity, struct ring_reverse_span spans[2])
{
	size_t end = newest + 1u;
	size_t head = ring_reverse_head_count(newest, count);

	spans[0] = (struct ring_reverse_span){ end - head, end };
	spans[1] = (struct ring_reverse_span){ capacity - (count - head), capacity };
}

/* Step backwards from a valid index in a nonempty ring. */
static inline size_t ring_index_previous(size_t index, size_t capacity)
{
	return index ? index - 1u : capacity - 1u;
}

/* Advance after storing at *write. Callers ensure *write < capacity and
 * *fill <= capacity; the fill saturates at capacity. */
static inline void ring_history_advance(
	size_t *write, size_t *fill, size_t capacity)
{
	if (++*write == capacity)
		*write = 0;
	if (*fill < capacity)
		++*fill;
}

#endif
