// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_QUALITY_H
#define V34_DATA_QUALITY_H

#include "v34_adaptive_equalizer.h"
#include "v34_info1.h"

#include "recovery.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_DATA_QUALITY_MAX_RETRAINS = 2,
	/* Local policy: require 32 clean windows before treating a later fault
	 * as a separate incident. These are measurement windows,
	 * not wall-clock ticks; skipped equalizer decisions contribute nothing. */
	V34_DATA_QUALITY_REARM_WINDOWS = 32,
};

enum v34_data_quality_result {
	V34_DATA_QUALITY_OK,
	V34_DATA_QUALITY_FREEZE_TIMING,
	V34_DATA_QUALITY_RESUME_TIMING,
	V34_DATA_QUALITY_RETRAIN,
	V34_DATA_QUALITY_INVALID_ARGUMENT,
	V34_DATA_QUALITY_INVALID_STATE,
};

/* Own the rate-relative online quality policy.  The adaptive equalizer owns
 * the measurements themselves; this compact owner consumes each completed
 * 1024-decision window exactly once and bounds automatic full retraining. */
struct v34_data_quality {
	struct recovery recovery;
	int16_t baseline;
	int16_t threshold;
	uint16_t window_position;
	uint8_t loss_windows;
	bool timing_frozen;
	bool active;
};

enum v34_data_quality_result v34_data_quality_init(
	struct v34_data_quality *quality);
bool v34_data_quality_is_valid(const struct v34_data_quality *quality);

/* Select the highest enabled receive rate supported by training quality.
 * Use the recent training window once available, otherwise the last online
 * window. A metric at or above a rate threshold steps down to the
 * next enabled rate; the lowest enabled rate remains the bounded fallback.
 * This does not change the measurement or the online recovery window. */
enum v34_data_quality_result v34_data_quality_select_receive_rate(
	enum v34_symbol_rate symbol_rate, uint16_t enabled_rate_mask,
	uint8_t maximum_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement,
	uint8_t *selected_rate_2400_bps_units);

/* Start a new data generation while retaining used automatic-retrain attempts,
 * not healthy credit. Only a subsequently completed window affects policy.
 * symbols_per_decision is 1 when measuring both symbols of each 4-D pair,
 * or 2 when measuring only its second symbol, as conventional V.34 does. */
enum v34_data_quality_result v34_data_quality_start(
	struct v34_data_quality *quality,
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement,
	unsigned int symbols_per_decision);

/* Observe after each single or paired decision. A window may finish between
 * the two decisions, so completion is a counter wrap, not an exact zero.
 * A completed window can first request that the trained sample clock be
 * frozen as quality approaches the rate-down threshold, then resume its
 * low-gain tracking profile after recovery. Return RETRAIN once consecutive
 * complete bad windows cover at least 1.8 seconds at the selected symbol rate.
 * The owner becomes inactive in that result and is restarted after retraining.
 * Once its two-retrain budget is
 * exhausted, later windows remain observable but cannot request recovery
 * until sustained clean measurements rearm the allowance. A disabled
 * threshold, bad window, or frozen timing breaks healthy credit. */
enum v34_data_quality_result v34_data_quality_observe(
	struct v34_data_quality *quality,
	const struct v34_adaptive_equalizer_quality *measurement);

#endif /* V34_DATA_QUALITY_H */
