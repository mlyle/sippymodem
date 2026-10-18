// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_quality.h"

#include "memory_region.h"
#include "v34_rate_profile.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_RATE_PROFILE_MASK = (1u << V34_RATE_PROFILE_UNITS) - 1u,
};

static uint16_t receive_rate_level(
	enum v34_symbol_rate symbol_rate, uint8_t rate_units)
{
	uint16_t level = 0;

	return v34_rate_profile_level(
		       symbol_rate, rate_units, true, &level) ==
		       V34_RATE_PROFILE_OK ?
		       level :
		       0;
}

static int16_t rate_threshold(uint16_t coefficient, uint8_t rate_units)
{
	int32_t scaled = coefficient >> 5;
	int32_t factor = rate_units < 13u ? 15000 : 17000;

	return (int16_t)(scaled * scaled * factor >> 14);
}

static bool selected_threshold(
	enum v34_symbol_rate symbol_rate, uint8_t rate_units,
	int16_t baseline, int16_t *threshold)
{
	uint8_t minimum_rate =
		symbol_rate == V34_SYMBOL_RATE_2400 ? 1u : 2u;
	uint16_t selected_level = receive_rate_level(symbol_rate, rate_units);
	int16_t selected;

	if (threshold == NULL || rate_units == 0 ||
	    rate_units < minimum_rate || rate_units > V34_RATE_PROFILE_UNITS ||
	    selected_level == 0)
		return false;
	if (rate_units == minimum_rate) {
		/* At the minimum rate there is no lower profile midpoint.  Use the
		 * completed training window as the receiver's quality baseline.  A
		 * saturated baseline disables recovery instead of wrapping the signed
		 * threshold into an immediate false retrain. */
		if (baseline > INT16_MAX / 8) {
			*threshold = 0;
			return true;
		}
		*threshold = (int16_t)(baseline * 8);
		return true;
	}
	selected = rate_threshold(selected_level, rate_units);
	selected_level = receive_rate_level(
		symbol_rate, (uint8_t)(rate_units - 1u));
	if (selected_level != 0) {
		int16_t lower = rate_threshold(
			selected_level, rate_units);

		selected = (int16_t)((selected + lower) >> 1);
	}
	*threshold = selected;
	return selected > 0;
}

static bool measurement_is_valid(
	const struct v34_adaptive_equalizer_quality *measurement)
{
	return measurement != NULL &&
	       measurement->window_position <
		       V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS &&
	       measurement->recent_symbol_count <=
		       V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS &&
	       measurement->recent_decision_metric >= 0 &&
	       measurement->decision_metric >= 0 &&
	       measurement->feedback_metric >= 0;
}

static uint8_t loss_windows(enum v34_symbol_rate symbol_rate)
{
	/* ceil(1.8 * baud / 1024), using exact fractional symbol rates.
	 * In particular, six windows at 24000/7 baud last only 1.792 s. */
	switch (symbol_rate) {
	case V34_SYMBOL_RATE_2400:
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_2800:
		return 5;
	case V34_SYMBOL_RATE_3000:
	case V34_SYMBOL_RATE_3200:
		return 6;
	case V34_SYMBOL_RATE_3429:
		return 7;
	default:
		return 0;
	}
}

bool v34_data_quality_is_valid(const struct v34_data_quality *quality)
{
	if (quality == NULL ||
	    (quality->loss_windows != 0 &&
	     (quality->loss_windows < 3 || quality->loss_windows > 7)) ||
	    quality->recovery.bad_windows > quality->loss_windows ||
	    quality->recovery.attempts > V34_DATA_QUALITY_MAX_RETRAINS ||
	    quality->recovery.good_windows >= V34_DATA_QUALITY_REARM_WINDOWS ||
	    (quality->recovery.good_windows != 0 &&
	     (!quality->active || quality->recovery.attempts == 0 ||
	      quality->recovery.bad_windows != 0 || quality->threshold == 0 ||
	      quality->timing_frozen)) ||
	    quality->window_position >= V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS ||
	    quality->baseline < 0 || quality->threshold < 0 ||
	    (quality->timing_frozen &&
	     (quality->threshold <= quality->baseline ||
	      quality->threshold == INT16_MAX)))
		return false;
	if (quality->active)
		return quality->loss_windows != 0;
	return quality->baseline == 0 && quality->threshold == 0 &&
	       quality->recovery.bad_windows == 0 && quality->window_position == 0 &&
	       !quality->timing_frozen;
}

enum v34_data_quality_result v34_data_quality_init(
	struct v34_data_quality *quality)
{
	if (quality == NULL)
		return V34_DATA_QUALITY_INVALID_ARGUMENT;
	*quality = (struct v34_data_quality){ 0 };
	return V34_DATA_QUALITY_OK;
}

static uint8_t next_enabled_rate(
	enum v34_symbol_rate symbol_rate, uint16_t enabled_rate_mask,
	uint8_t maximum_rate)
{
	for (uint8_t rate = maximum_rate; rate != 0u; --rate)
		if ((enabled_rate_mask & (uint16_t)(1u << (rate - 1u))) != 0u &&
		    receive_rate_level(symbol_rate, rate) != 0u)
			return rate;
	return 0u;
}

enum v34_data_quality_result v34_data_quality_select_receive_rate(
	enum v34_symbol_rate symbol_rate, uint16_t enabled_rate_mask,
	uint8_t maximum_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement,
	uint8_t *selected_rate_2400_bps_units)
{
	uint8_t selected;
	int16_t metric;

	if (measurement == NULL ||
	    selected_rate_2400_bps_units == NULL ||
	    memory_regions_overlap(
		measurement, sizeof(*measurement),
		selected_rate_2400_bps_units,
		sizeof(*selected_rate_2400_bps_units)) ||
	    !measurement_is_valid(measurement) || enabled_rate_mask == 0u ||
	    (enabled_rate_mask & (uint16_t)~V34_RATE_PROFILE_MASK) != 0u ||
	    maximum_rate_2400_bps_units == 0u ||
	    maximum_rate_2400_bps_units > V34_RATE_PROFILE_UNITS)
		return V34_DATA_QUALITY_INVALID_ARGUMENT;
	selected = next_enabled_rate(
		symbol_rate, enabled_rate_mask, maximum_rate_2400_bps_units);
	if (selected == 0u)
		return V34_DATA_QUALITY_INVALID_ARGUMENT;
	metric = measurement->recent_symbol_count ==
			 V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS ?
		 measurement->recent_decision_metric : measurement->decision_metric;
	for (;;) {
		uint8_t lower = next_enabled_rate(
			symbol_rate, enabled_rate_mask, (uint8_t)(selected - 1u));
		int16_t threshold = rate_threshold(
			receive_rate_level(symbol_rate, selected), selected);

		if (lower == 0u || metric < threshold)
			break;
		selected = lower;
	}
	*selected_rate_2400_bps_units = selected;
	return V34_DATA_QUALITY_OK;
}

enum v34_data_quality_result v34_data_quality_start(
	struct v34_data_quality *quality,
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement,
	unsigned int symbols_per_decision)
{
	struct v34_data_quality next;
	int16_t threshold;
	int32_t relaxed_threshold;

	if (quality == NULL || measurement == NULL ||
	    (symbols_per_decision != 1u && symbols_per_decision != 2u) ||
	    memory_regions_overlap(
		quality, sizeof(*quality), measurement, sizeof(*measurement)) ||
	    !measurement_is_valid(measurement) ||
	    !selected_threshold(
		symbol_rate, primary_data_rate_2400_bps_units,
		measurement->decision_metric, &threshold))
		return V34_DATA_QUALITY_INVALID_ARGUMENT;
	if (!v34_data_quality_is_valid(quality))
		return V34_DATA_QUALITY_INVALID_STATE;
	next = *quality;
	next.baseline = measurement->decision_metric;
	/* Give online recovery 5% headroom, rounded upward. Training rate
	 * selection retains its original thresholds. Preserve disabled recovery
	 * and saturate high minimum-rate baselines instead of overflowing. */
	relaxed_threshold = ((int32_t)threshold * 105 + 99) / 100;
	next.threshold = (int16_t)(relaxed_threshold > INT16_MAX ?
				   INT16_MAX : relaxed_threshold);
	/* Each window contains 1024 measured decisions. Some receivers adapt
	 * only on the second symbol of each pair, doubling its duration. */
	next.loss_windows = (uint8_t)((loss_windows(symbol_rate) +
		symbols_per_decision - 1u) / symbols_per_decision);
	next.recovery.bad_windows = 0;
	next.recovery.good_windows = 0;
	next.window_position = measurement->window_position;
	next.timing_frozen = false;
	next.active = true;
	*quality = next;
	return V34_DATA_QUALITY_OK;
}

static enum v34_data_quality_result update_timing_gate(
	struct v34_data_quality *quality, int16_t metric)
{
	int16_t range;
	int16_t freeze_threshold;
	int16_t resume_threshold;

	if (quality->threshold <= quality->baseline ||
	    quality->threshold == INT16_MAX)
		return V34_DATA_QUALITY_OK;
	range = (int16_t)(quality->threshold - quality->baseline);
	freeze_threshold = (int16_t)(
		quality->baseline + (int16_t)(((int32_t)range * 3) >> 3));
	resume_threshold = (int16_t)(quality->baseline + (range >> 2));
	if (!quality->timing_frozen && metric >= freeze_threshold) {
		quality->timing_frozen = true;
		return V34_DATA_QUALITY_FREEZE_TIMING;
	}
	if (quality->timing_frozen && metric <= resume_threshold) {
		quality->timing_frozen = false;
		return V34_DATA_QUALITY_RESUME_TIMING;
	}
	return V34_DATA_QUALITY_OK;
}

enum v34_data_quality_result v34_data_quality_observe(
	struct v34_data_quality *quality,
	const struct v34_adaptive_equalizer_quality *measurement)
{
	struct v34_data_quality next;
	enum v34_data_quality_result timing_result = V34_DATA_QUALITY_OK;

	if (quality == NULL || measurement == NULL ||
	    memory_regions_overlap(
		quality, sizeof(*quality), measurement, sizeof(*measurement)) ||
	    !measurement_is_valid(measurement))
		return V34_DATA_QUALITY_INVALID_ARGUMENT;
	if (!v34_data_quality_is_valid(quality) || !quality->active)
		return V34_DATA_QUALITY_INVALID_STATE;
	next = *quality;
	if (measurement->window_position < next.window_position) {
		timing_result = update_timing_gate(
			&next, measurement->decision_metric);
		bool bad = next.threshold != 0 &&
			measurement->decision_metric >= next.threshold;

		recovery_window(&next.recovery, bad,
			next.threshold != 0 && !bad && !next.timing_frozen,
			next.loss_windows, V34_DATA_QUALITY_REARM_WINDOWS);
	}
	next.window_position = measurement->window_position;
	if (next.recovery.bad_windows == next.loss_windows &&
	    recovery_take(&next.recovery, V34_DATA_QUALITY_MAX_RETRAINS)) {
		next.baseline = 0;
		next.threshold = 0;
		next.window_position = 0;
		next.timing_frozen = false;
		next.active = false;
		*quality = next;
		return V34_DATA_QUALITY_RETRAIN;
	}
	*quality = next;
	return timing_result;
}
