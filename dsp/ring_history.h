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
