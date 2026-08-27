// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_quality.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_RATE_PROFILE_UNITS = 14,
	V34_RATE_PROFILE_MASK = (1u << V34_RATE_PROFILE_UNITS) - 1u,
};

/* Receive-rate coefficients for the decision metric's fixed-point domain.
 * The 2743-symbol/s mode uses the 2800-symbol/s profile, matching its shared
 * carrier/rate-selection geometry.  Only the active receive half of each
 * profile is needed by online quality policy. */
static const uint16_t scale_2400[V34_RATE_PROFILE_UNITS] = {
	0x11e3, 0x0db7, 0x0a10, 0x0719, 0x0504, 0x0391, 0x0286,
	0x01c8, 0x0143, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

static const uint16_t scale_2800[V34_RATE_PROFILE_UNITS] = {
	0x0000, 0x0f7d, 0x0b94, 0x08a3, 0x0677, 0x04d6, 0x0392,
	0x02aa, 0x01fb, 0x0178, 0x0118, 0x0000, 0x0000, 0x0000,
};

static const uint16_t scale_3000[V34_RATE_PROFILE_UNITS] = {
	0x0000, 0x103b, 0x0c33, 0x0955, 0x0714, 0x056f, 0x041c,
	0x031f, 0x025e, 0x01c8, 0x015c, 0x0107, 0x0000, 0x0000,
};

static const uint16_t scale_3200[V34_RATE_PROFILE_UNITS] = {
	0x0000, 0x11e3, 0x0c43, 0x0a0c, 0x07c1, 0x05f6, 0x04a5,
	0x0393, 0x02c2, 0x0220, 0x01a4, 0x0143, 0x00fa, 0x0000,
};

static const uint16_t scale_3429[V34_RATE_PROFILE_UNITS] = {
	0x0000, 0x11e3, 0x0d4b, 0x0aaa, 0x0862, 0x06a3, 0x053f,
	0x041f, 0x0339, 0x0285, 0x01fd, 0x0190, 0x0137, 0x00f6,
};

static const uint16_t *rate_profile(enum v34_symbol_rate symbol_rate)
{
	switch (symbol_rate) {
	case V34_SYMBOL_RATE_2400:
		return scale_2400;
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_2800:
		return scale_2800;
	case V34_SYMBOL_RATE_3000:
		return scale_3000;
	case V34_SYMBOL_RATE_3200:
		return scale_3200;
	case V34_SYMBOL_RATE_3429:
		return scale_3429;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return NULL;
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
	const uint16_t *profile = rate_profile(symbol_rate);
	uint8_t minimum_rate =
		symbol_rate == V34_SYMBOL_RATE_2400 ? 1u : 2u;
	int16_t selected;

	if (profile == NULL || threshold == NULL || rate_units == 0 ||
	    rate_units < minimum_rate || rate_units > V34_RATE_PROFILE_UNITS ||
	    profile[rate_units - 1u] == 0)
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
	selected = rate_threshold(profile[rate_units - 1u], rate_units);
	if (profile[rate_units - 2u] != 0) {
		int16_t lower = rate_threshold(
			profile[rate_units - 2u], rate_units);

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
	       measurement->decision_metric >= 0 &&
	       measurement->feedback_metric >= 0;
}

bool v34_data_quality_is_valid(const struct v34_data_quality *quality)
{
	if (quality == NULL ||
	    quality->bad_windows > V34_DATA_QUALITY_LOSS_WINDOWS ||
	    quality->automatic_retrains > V34_DATA_QUALITY_MAX_RETRAINS ||
	    quality->baseline < 0 || quality->threshold < 0 ||
	    (quality->timing_frozen &&
	     (quality->threshold <= quality->baseline ||
	      quality->threshold == INT16_MAX)))
		return false;
	if (quality->active)
		return true;
	return quality->baseline == 0 && quality->threshold == 0 &&
	       quality->bad_windows == 0 && !quality->window_latched &&
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
	const uint16_t *profile, uint16_t enabled_rate_mask,
	uint8_t maximum_rate)
{
	for (uint8_t rate = maximum_rate; rate != 0u; --rate)
		if ((enabled_rate_mask & (uint16_t)(1u << (rate - 1u))) != 0u &&
		    profile[rate - 1u] != 0u)
			return rate;
	return 0u;
}

enum v34_data_quality_result v34_data_quality_select_receive_rate(
	enum v34_symbol_rate symbol_rate, uint16_t enabled_rate_mask,
	uint8_t maximum_rate_2400_bps_units,
	const struct v34_adaptive_equalizer_quality *measurement,
	uint8_t *selected_rate_2400_bps_units)
{
	const uint16_t *profile = rate_profile(symbol_rate);
	uint8_t selected;

	if (profile == NULL || measurement == NULL ||
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
		profile, enabled_rate_mask, maximum_rate_2400_bps_units);
	if (selected == 0u)
		return V34_DATA_QUALITY_INVALID_ARGUMENT;
	for (;;) {
		uint8_t lower = next_enabled_rate(
			profile, enabled_rate_mask, (uint8_t)(selected - 1u));
		int16_t threshold = rate_threshold(
			profile[selected - 1u], selected);

		if (lower == 0u || measurement->decision_metric < threshold)
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
	const struct v34_adaptive_equalizer_quality *measurement)
{
	struct v34_data_quality next;
	int16_t threshold;

	if (quality == NULL || measurement == NULL ||
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
	next.threshold = threshold;
	next.bad_windows = 0;
	next.window_latched = measurement->window_position == 0;
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
	if (measurement->window_position != 0) {
		next.window_latched = false;
	} else if (!next.window_latched) {
		next.window_latched = true;
		timing_result = update_timing_gate(
			&next, measurement->decision_metric);
		if (next.threshold == 0 ||
		    measurement->decision_metric < next.threshold) {
			next.bad_windows = 0;
		} else if (next.bad_windows < V34_DATA_QUALITY_LOSS_WINDOWS) {
			next.bad_windows++;
		}
	}
	if (next.bad_windows == V34_DATA_QUALITY_LOSS_WINDOWS &&
	    next.automatic_retrains < V34_DATA_QUALITY_MAX_RETRAINS) {
		next.automatic_retrains++;
		next.baseline = 0;
		next.threshold = 0;
		next.bad_windows = 0;
		next.window_latched = false;
		next.timing_frozen = false;
		next.active = false;
		*quality = next;
		return V34_DATA_QUALITY_RETRAIN;
	}
	*quality = next;
	return timing_result;
}
