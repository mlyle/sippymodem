// SPDX-License-Identifier: BSD-3-Clause
#ifndef FRAME_PACER_H
#define FRAME_PACER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Each frame spends interval_ns of credit. Elapsed monotonic time restores
 * credit up to the burst bound, so idle time cannot fund a larger burst. */
struct frame_pacer {
	uint64_t interval_ns;
	uint64_t capacity_ns;
	uint64_t credit_ns;
	uint64_t last_time_ns;
	bool started;
};

bool frame_pacer_init(struct frame_pacer *pacer,
	uint64_t interval_ns, size_t maximum_burst_frames);
void frame_pacer_reset(struct frame_pacer *pacer);
bool frame_pacer_take(struct frame_pacer *pacer, uint64_t now_ns);
uint64_t frame_pacer_wait_ns(const struct frame_pacer *pacer, uint64_t now_ns);

#endif
