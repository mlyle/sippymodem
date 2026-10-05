// SPDX-License-Identifier: BSD-3-Clause
#include "byte_queue.h"

#include <string.h>

size_t byte_queue_write(struct byte_queue *queue, const void *bytes, size_t count)
{
	if (count > queue->capacity - queue->count)
		count = queue->capacity - queue->count;
	if (count == 0)
		return 0;
	size_t tail = (queue->head + queue->count) % queue->capacity;
	size_t first = count < queue->capacity - tail ? count : queue->capacity - tail;
	memcpy(queue->storage + tail, bytes, first);
	memcpy(queue->storage, (const uint8_t *)bytes + first, count - first);
	queue->count += count;
	return count;
}

size_t byte_queue_read(struct byte_queue *queue, void *bytes, size_t capacity)
{
	size_t count = capacity < queue->count ? capacity : queue->count;
	if (count == 0)
		return 0;
	size_t first = count < queue->capacity - queue->head ? count : queue->capacity - queue->head;
	memcpy(bytes, queue->storage + queue->head, first);
	memcpy((uint8_t *)bytes + first, queue->storage, count - first);
	queue->head = (queue->head + count) % queue->capacity;
	queue->count -= count;
	return count;
}
