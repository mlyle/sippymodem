// SPDX-License-Identifier: BSD-3-Clause
#ifndef BYTE_QUEUE_H
#define BYTE_QUEUE_H

#include <stddef.h>
#include <stdint.h>

/* The owner supplies storage and serializes every operation. Counts refer to
 * local bytes; read/write copy exactly the prefix they return. */
struct byte_queue {
	uint8_t *storage;
	size_t capacity;
	size_t head;
	size_t count;
};

size_t byte_queue_write(struct byte_queue *queue, const void *bytes, size_t count);
size_t byte_queue_read(struct byte_queue *queue, void *bytes, size_t capacity);

#endif
