// SPDX-License-Identifier: BSD-3-Clause
#include "log_buffer.h"
#include "test.h"
#include <string.h>

static bool test_log_buffer(void)
{
	struct sippymodem_log_record storage[3], records[3];
	struct log_buffer buffer = { .storage = storage, .capacity = 3 };
	struct sippymodem_log_record input = {
		.time_ns = 100, .generation = 7, .line_id = 9,
		.level = SIPPYMODEM_LOG_WARNING, .truncated = true, .text = "original",
	};
	uint64_t cursor = 0, independent = 0, missed = 99;
	size_t count = 99;
	CHECK(log_buffer_read(&buffer, &cursor, records, 3, &count, &missed) == SIPPYMODEM_AGAIN &&
	      count == 0 && missed == 0, "empty buffer returned records");
	for (unsigned i = 0; i < 5; ++i)
		CHECK(log_buffer_append(&buffer, &input), "append failed");
	strcpy(input.text, "changed");
	CHECK(log_buffer_read(&buffer, &cursor, records, 1, &count, &missed) == SIPPYMODEM_OK &&
	      count == 1 && missed == 2 && cursor == 3, "wrap gap or partial count is wrong");
	CHECK(records[0].sequence == 3 && records[0].time_ns == 100 && records[0].generation == 7 &&
	      records[0].line_id == 9 && records[0].level == SIPPYMODEM_LOG_WARNING &&
	      records[0].truncated && strcmp(records[0].text, "original") == 0, "record was not copied");
	CHECK(log_buffer_read(&buffer, &independent, records, 3, &count, &missed) == SIPPYMODEM_OK &&
	      count == 3 && missed == 2 && independent == 5 && cursor == 3, "cursors are not independent");
	for (size_t i = 0; i < count; ++i)
		CHECK(records[i].sequence == i + 3, "records are out of order");
	CHECK(log_buffer_read(&buffer, &cursor, NULL, 0, &count, &missed) == SIPPYMODEM_OK &&
	      cursor == 3 && count == 0 && missed == 0, "zero capacity advanced cursor");
	CHECK(log_buffer_read(&buffer, &cursor, records, 3, &count, &missed) == SIPPYMODEM_OK &&
	      count == 2 && missed == 0 && cursor == 5, "partial remainder is wrong");
	CHECK(log_buffer_read(&buffer, &cursor, records, 3, &count, &missed) == SIPPYMODEM_AGAIN,
	      "already-read records returned again");
	cursor = 6;
	CHECK(log_buffer_read(&buffer, &cursor, records, 3, &count, &missed) == SIPPYMODEM_STALE &&
	      cursor == 6 && count == 0 && missed == 0, "future cursor was accepted");
	CHECK(log_buffer_read(NULL, &cursor, records, 3, &count, &missed) == SIPPYMODEM_INVALID &&
	      count == 0 && missed == 0, "invalid buffer accepted");
	CHECK(log_buffer_read(&buffer, NULL, records, 3, &count, &missed) == SIPPYMODEM_INVALID &&
	      log_buffer_read(&buffer, &cursor, NULL, 1, &count, &missed) == SIPPYMODEM_INVALID &&
	      log_buffer_read(&buffer, &cursor, records, 3, NULL, &missed) == SIPPYMODEM_INVALID &&
	      log_buffer_read(&buffer, &cursor, records, 3, &count, NULL) == SIPPYMODEM_INVALID,
	      "missing output accepted");
	CHECK(!log_buffer_append(NULL, &input) && !log_buffer_append(&buffer, NULL), "invalid append accepted");
	struct log_buffer disabled = { 0 };
	CHECK(!log_buffer_append(&disabled, &input), "disabled buffer accepted record");
	/* Sequence arithmetic must not wrap into a previous call's evidence. */
	buffer.count = 0;
	buffer.sequence = UINT64_MAX - 1;
	cursor = UINT64_MAX - 1;
	CHECK(log_buffer_append(&buffer, &input) && !log_buffer_append(&buffer, &input), "sequence wrapped");
	CHECK(log_buffer_read(&buffer, &cursor, records, 3, &count, &missed) == SIPPYMODEM_OK &&
	      count == 1 && cursor == UINT64_MAX && records[0].sequence == UINT64_MAX, "last sequence lost");
	return true;
}

int main(void)
{
	return test_log_buffer() ? 0 : 1;
}
