// SPDX-License-Identifier: BSD-3-Clause
#include "log_buffer.h"

bool log_buffer_append(struct log_buffer *buffer, const struct sippymodem_log_record *record)
{
	if (buffer == NULL || record == NULL || buffer->storage == NULL ||
	    buffer->capacity == 0 || buffer->sequence == UINT64_MAX)
		return false;
	uint64_t sequence = ++buffer->sequence;
	struct sippymodem_log_record *slot = &buffer->storage[(sequence - 1) % buffer->capacity];
	*slot = *record;
	slot->sequence = sequence;
	if (buffer->count < buffer->capacity)
		buffer->count++;
	return true;
}

enum sippymodem_result log_buffer_read(const struct log_buffer *buffer,
	uint64_t *cursor, struct sippymodem_log_record *records, size_t capacity,
	size_t *count, uint64_t *missed)
{
	if (count != NULL)
		*count = 0;
	if (missed != NULL)
		*missed = 0;
	if (buffer == NULL || cursor == NULL || count == NULL || missed == NULL ||
	    (capacity != 0 && records == NULL))
		return SIPPYMODEM_INVALID;
	if (*cursor > buffer->sequence)
		return SIPPYMODEM_STALE;
	if (capacity == 0)
		return SIPPYMODEM_OK;
	if (*cursor == buffer->sequence || buffer->count == 0)
		return SIPPYMODEM_AGAIN;
	uint64_t first = *cursor + 1;
	uint64_t oldest = buffer->sequence - buffer->count + 1;
	if (first < oldest) {
		*missed = oldest - first;
		first = oldest;
	}
	size_t available = (size_t)(buffer->sequence - first + 1);
	if (available > capacity)
		available = capacity;
	for (size_t i = 0; i < available; ++i)
		records[i] = buffer->storage[(first + i - 1) % buffer->capacity];
	*count = available;
	*cursor = first + (available - 1);
	return SIPPYMODEM_OK;
}
