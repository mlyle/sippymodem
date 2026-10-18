// SPDX-License-Identifier: BSD-3-Clause

#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

/* Call-owned recovery accounting. Protocol owners supply completed windows,
 * their limits, and whether a window may earn healthy credit. Restarting a
 * receiver clears its window counters while retaining used attempts. */
struct recovery {
	uint16_t good_windows;
	uint8_t bad_windows;
	uint8_t attempts;
};

/* The owner supplies valid counters and positive limits. Disabled recovery,
 * ambiguity, or a frozen clock can prohibit healthy credit without counting
 * a bad window. */
static inline void recovery_window(struct recovery *recovery,
	bool bad, bool healthy, uint8_t bad_limit, uint16_t rearm_windows)
{
	if (!bad)
		recovery->bad_windows = 0;
	else if (recovery->bad_windows < bad_limit)
		++recovery->bad_windows;
	if (bad || !healthy) {
		recovery->good_windows = 0;
	} else if (recovery->attempts && ++recovery->good_windows == rearm_windows) {
		recovery->attempts = 0;
		recovery->good_windows = 0;
	}
}

static inline bool recovery_take(struct recovery *recovery, uint8_t limit)
{
	if (recovery->attempts >= limit)
		return false;
	*recovery = (struct recovery){ .attempts = recovery->attempts + 1u };
	return true;
}

#endif
