// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_QUALITY_H
#define V34_DATA_QUALITY_H

#include "v34_adaptive_equalizer.h"
#include "v34_info1.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_DATA_QUALITY_LOSS_WINDOWS = 3,
	V34_DATA_QUALITY_MAX_RETRAINS = 2,
};

enum v34_data_quality_result {
	V34_DATA_QUALITY_OK,
	V34_DATA_QUALITY_RETRAIN,
	V34_DATA_QUALITY_INVALID_ARGUMENT,
	V34_DATA_QUALITY_INVALID_STATE,
};

/* Own the rate-relative online quality policy.  The adaptive equalizer owns
 * the measurements themselves; this compact owner consumes each completed
 * 1024-symbol window exactly once and bounds automatic full retraining. */
struct v34_data_quality {
	int16_t threshold;
	uint8_t bad_windows;
	uint8_t automatic_retrains;
	bool window_latched;
	bool active;
};

enum v34_data_quality_result v34_data_quality_init(
	struct v34_data_quality *quality);
bool v34_data_quality_is_valid(const struct v34_data_quality *quality);

/* Select the highest enabled receive rate supported by the completed
 * training metric.  A metric at or above a rate threshold steps down to the
 * next enabled rate; the lowest enabled rate remains the bounded fallback. */
enum v34_data_quality_result v34_data_quality_select_receive_rate(
	enum v34_symbol_rate symbol_rate, uint16_t enabled_rate_mask,
	uint8_t maximum_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement,
	uint8_t *selected_rate_2400_bps_units);

/* Start a new data generation while retaining the automatic-retrain budget.
 * The current window is deliberately latched when it ends exactly at the B1
 * boundary; only a subsequently completed online window affects policy. */
enum v34_data_quality_result v34_data_quality_start(
	struct v34_data_quality *quality,
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement);

/* Return RETRAIN once after three consecutive complete bad windows.  The
 * owner becomes inactive in that result and is restarted after retraining.
 * Once its two-retrain budget is exhausted, later windows remain observable
 * but do not repeatedly request an impossible recovery. */
enum v34_data_quality_result v34_data_quality_observe(
	struct v34_data_quality *quality,
	const struct v34_adaptive_equalizer_quality *measurement);

#endif /* V34_DATA_QUALITY_H */
