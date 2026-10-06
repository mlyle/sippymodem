// SPDX-License-Identifier: BSD-3-Clause
#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include "sippymodem.h"

/* Storage and every operation belong to one native owner. */
struct log_buffer {
	struct sippymodem_log_record *storage;
	size_t capacity, count;
	uint64_t sequence;
};
bool log_buffer_append(struct log_buffer *buffer, const struct sippymodem_log_record *record);
enum sippymodem_result log_buffer_read(const struct log_buffer *buffer,
	uint64_t *cursor, struct sippymodem_log_record *records, size_t capacity,
	size_t *count, uint64_t *missed);

#endif
