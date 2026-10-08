// SPDX-License-Identifier: BSD-3-Clause
#ifndef SDK_IO_TEST_H
#define SDK_IO_TEST_H

#include "sippymodem.h"
#include "test.h"

/* Successful-write tests must allow a line's current DSP job to finish. */
static inline bool send_when_available(struct sippymodem_server *server, uint64_t *now,
	struct sippymodem_line *line, const void *bytes, size_t length, size_t *accepted,
	bool (*advance)(struct sippymodem_server *, uint64_t *))
{
	for (unsigned int index = 0; index < 30000; ++index) {
		enum sippymodem_result result = sippymodem_send(line, bytes, length, accepted);
		if (result != SIPPYMODEM_AGAIN) {
			CHECK(result == SIPPYMODEM_OK, "send failed: %d", result);
			return true;
		}
		CHECK(*accepted == 0, "busy send accepted bytes");
		CHECK(advance(server, now), "advance while send is busy");
	}
	return false;
}

#endif
