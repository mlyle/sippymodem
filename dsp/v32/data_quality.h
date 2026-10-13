// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_DATA_QUALITY_H
#define V32_DATA_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_QUALITY_WINDOW_SYMBOLS = 240, /* 100 ms at 2400 baud. */
	V32_QUALITY_WARMUP_WINDOWS = 10,
	V32_QUALITY_BAD_WINDOWS = 18, /* At least 1.8 seconds. */
	V32_QUALITY_REARM_WINDOWS = 300,
	V32_QUALITY_MAX_RETRAINS = 2,
};

/* The call owns the recovery budget across receiver restarts. */
struct v32_data_quality {
	uint32_t error_sum;
	uint16_t threshold;
	uint16_t window_symbols;
	uint16_t good_windows;
	uint8_t warmup_windows;
	uint8_t bad_windows;
	uint8_t automatic_retrains;
};

/* A zero-initialized owner starts a new call. Starting after B1 preserves
 * the recovery budget and gives the newly trained receiver one second. */
bool v32_data_quality_start(struct v32_data_quality *quality, unsigned bit_rate);

/* Account only for new equalizer decisions, never repeated media polls.
 * One update may contain a bounded group released by symbol timing. */
bool v32_data_quality_observe(struct v32_data_quality *quality,
	int16_t decision_error, unsigned symbols);

#endif
