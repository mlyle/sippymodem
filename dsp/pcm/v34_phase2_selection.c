// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_selection.h"

#include "memory_region.h"

#include <math.h>
#include <stddef.h>

enum {
	V34_PREEMPHASIS_THRESHOLD_BIN = 4,
	V34_PREEMPHASIS_2400_2800_BIN = 18,
	V34_PREEMPHASIS_3000_BIN = 19,
	V34_PREEMPHASIS_3200_BIN = 20,
	V34_PREEMPHASIS_3429_BIN = 22,
	V34_PREEMPHASIS_FIRST_FILTER = 6,
};

struct preemphasis_probe_profile {
	uint8_t source_bin;
	uint16_t slope_q14;
};

uint8_t v34_phase2_projected_rate_limit(
	enum v34_symbol_rate symbol_rate)
{
	static const uint8_t limits[V34_SYMBOL_RATE_COUNT] = {
		9, 10, 11, 12, 13, 14,
	};

	return (unsigned int)symbol_rate < V34_SYMBOL_RATE_COUNT ?
		       limits[symbol_rate] : 0;
}

static bool preemphasis_probe_profile(
	enum v34_symbol_rate symbol_rate,
	struct preemphasis_probe_profile *profile)
{
	static const struct preemphasis_probe_profile profiles[] = {
		[V34_SYMBOL_RATE_2400] = {
			V34_PREEMPHASIS_2400_2800_BIN, 0x7da7,
		},
		[V34_SYMBOL_RATE_2743] = {
			V34_PREEMPHASIS_2400_2800_BIN, 0x6789,
		},
		[V34_SYMBOL_RATE_2800] = {
			V34_PREEMPHASIS_2400_2800_BIN, 0x6789,
		},
		[V34_SYMBOL_RATE_3000] = {
			V34_PREEMPHASIS_3000_BIN, 0x656f,
		},
		[V34_SYMBOL_RATE_3200] = {
			V34_PREEMPHASIS_3200_BIN, 0x639f,
		},
		[V34_SYMBOL_RATE_3429] = {
			V34_PREEMPHASIS_3429_BIN, 0x6626,
		},
	};

	if (profile == NULL || (unsigned int)symbol_rate >=
				       V34_SYMBOL_RATE_COUNT)
		return false;
	*profile = profiles[symbol_rate];
	return true;
}

enum v34_phase2_selection_result v34_phase2_select_preemphasis(
	const struct v34_phase2_probe_receiver_measurement *measurement,
	enum v34_symbol_rate symbol_rate, uint8_t *filter_index)
{
	struct preemphasis_probe_profile profile;
	double sample;
	double threshold;
	double slope;
	uint8_t selected = V34_PREEMPHASIS_FIRST_FILTER - 1u;

	if (measurement == NULL || filter_index == NULL ||
	    memory_regions_overlap(
		    measurement, sizeof(*measurement), filter_index,
		    sizeof(*filter_index)) ||
	    !preemphasis_probe_profile(symbol_rate, &profile) ||
	    measurement->l2.sample_count != V34_PHASE2_PROBE_L1_SAMPLES ||
	    measurement->l2_samples < V34_PHASE2_PROBE_L1_SAMPLES ||
	    measurement->l2_samples >=
		    V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES)
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	sample = measurement->l2.harmonics[profile.source_bin].mean_square;
	threshold = measurement->l2
			    .harmonics[V34_PREEMPHASIS_THRESHOLD_BIN]
			    .mean_square;
	if (!isfinite(sample) || sample < 0.0 || !isfinite(threshold) ||
	    threshold < 0.0)
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	slope = (double)profile.slope_q14 / 16384.0;
	do {
		selected++;
		sample *= slope;
		if (!isfinite(sample))
			return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
		if (threshold < sample)
			break;
	} while (selected < V34_INFO1_MAX_PREEMPHASIS_FILTER_INDEX);
	*filter_index = selected;
	return V34_PHASE2_SELECTION_OK;
}

static bool receiver_supports_rate(
	const struct v34_info0 *receiver, enum v34_symbol_rate rate)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
	case V34_SYMBOL_RATE_3000:
	case V34_SYMBOL_RATE_3200:
		return true;
	case V34_SYMBOL_RATE_2743:
		return receiver->symbol_rate_2743_supported;
	case V34_SYMBOL_RATE_2800:
		return receiver->symbol_rate_2800_supported;
	case V34_SYMBOL_RATE_3429:
		return receiver->symbol_rate_3429_supported;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static bool transmitter_supports_rate(
	const struct v34_info0 *transmitter, enum v34_symbol_rate rate,
	bool high_carrier)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		return true;
	case V34_SYMBOL_RATE_2743:
		return transmitter->symbol_rate_2743_supported;
	case V34_SYMBOL_RATE_2800:
		return transmitter->symbol_rate_2800_supported;
	case V34_SYMBOL_RATE_3000:
		return high_carrier ?
			transmitter->high_carrier_3000_supported :
			transmitter->low_carrier_3000_supported;
	case V34_SYMBOL_RATE_3200:
		return high_carrier ?
			transmitter->high_carrier_3200_supported :
			transmitter->low_carrier_3200_supported;
	case V34_SYMBOL_RATE_3429:
		return transmitter->symbol_rate_3429_supported &&
		       transmitter->symbol_rate_3429_transmit_permitted;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static bool power_request_is_consistent(
	const struct v34_info1_common *request,
	const struct v34_info0 *transmitter)
{
	return transmitter->transmit_power_reduction_supported ||
	       (request->minimum_peer_power_reduction_db == 0 &&
		request->additional_peer_power_reduction_db == 0);
}

static bool probe_result_is_consistent(
	const struct v34_info1_probe_result *probe,
	enum v34_symbol_rate rate, const struct v34_info0 *transmitter,
	const struct v34_info0 *receiver)
{
	if (probe->projected_data_rate_2400_bps_units == 0)
		return true;
	return transmitter_supports_rate(
		       transmitter, rate, probe->high_carrier) &&
	       receiver_supports_rate(receiver, rate) &&
	       (probe->projected_data_rate_2400_bps_units <= 12 ||
		transmitter->constellation_1664_supported);
}

bool v34_phase2_info1c_is_consistent(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c)
{
	if (!v34_info0_is_valid(caller_info0) ||
	    !v34_info0_is_valid(answerer_info0) ||
	    !v34_info1c_is_valid(info1c) ||
	    !power_request_is_consistent(
		    &info1c->common, answerer_info0))
		return false;
	for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index) {
		if (!probe_result_is_consistent(
			    &info1c->answer_transmit[index],
			    (enum v34_symbol_rate)index, answerer_info0,
			    caller_info0))
			return false;
	}
	return true;
}

static unsigned int symbol_rate_difference(
	enum v34_symbol_rate first, enum v34_symbol_rate second)
{
	return first > second ? (unsigned int)first - (unsigned int)second :
				(unsigned int)second - (unsigned int)first;
}

bool v34_phase2_info1a_is_consistent(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *info1a)
{
	const struct v34_info1_probe_result *answer_transmit;
	unsigned int rate_difference;

	if (!v34_phase2_info1c_is_consistent(
		    caller_info0, answerer_info0, info1c) ||
	    !v34_info1a_is_valid(info1a) ||
	    !power_request_is_consistent(
		    &info1a->common, caller_info0))
		return false;
	answer_transmit =
		&info1c->answer_transmit[
			info1a->answer_to_call_symbol_rate];
	if (answer_transmit->projected_data_rate_2400_bps_units == 0 ||
	    info1a->call_transmit.projected_data_rate_2400_bps_units == 0 ||
	    !probe_result_is_consistent(
		    &info1a->call_transmit,
		    info1a->call_to_answer_symbol_rate,
		    caller_info0, answerer_info0))
		return false;
	rate_difference = symbol_rate_difference(
		info1a->answer_to_call_symbol_rate,
		info1a->call_to_answer_symbol_rate);
	return rate_difference <=
		       caller_info0->maximum_symbol_rate_difference_steps &&
	       rate_difference <=
		       answerer_info0->maximum_symbol_rate_difference_steps;
}

static bool select_call_transmit_carrier(
	const struct v34_info0 *caller_info0, enum v34_symbol_rate rate,
	bool preferred, bool *selected)
{
	if (transmitter_supports_rate(caller_info0, rate, preferred)) {
		*selected = preferred;
		return true;
	}
	if (transmitter_supports_rate(caller_info0, rate, !preferred)) {
		*selected = !preferred;
		return true;
	}
	return false;
}

enum v34_phase2_selection_result v34_phase2_select_answerer_info1a(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *preferred,
	struct v34_info1a *selected)
{
	struct v34_info1a candidate;
	unsigned int ceiling;

	if (caller_info0 == NULL || answerer_info0 == NULL || info1c == NULL ||
	    preferred == NULL || selected == NULL ||
	    memory_regions_overlap(
		    selected, sizeof(*selected), caller_info0,
		    sizeof(*caller_info0)) ||
	    memory_regions_overlap(
		    selected, sizeof(*selected), answerer_info0,
		    sizeof(*answerer_info0)) ||
	    memory_regions_overlap(
		    selected, sizeof(*selected), info1c, sizeof(*info1c)) ||
	    memory_regions_overlap(
		    selected, sizeof(*selected), preferred,
		    sizeof(*preferred)) ||
	    !v34_info0_is_valid(caller_info0) ||
	    !v34_info0_is_valid(answerer_info0) ||
	    !v34_info1c_is_valid(info1c) ||
	    !v34_info1a_is_valid(preferred))
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	if (!v34_phase2_info1c_is_consistent(
		    caller_info0, answerer_info0, info1c))
		return V34_PHASE2_SELECTION_INCONSISTENT_INFO1C;
	if (!power_request_is_consistent(
		    &preferred->common, caller_info0) ||
	    preferred->call_transmit.projected_data_rate_2400_bps_units == 0)
		return V34_PHASE2_SELECTION_INCONSISTENT_INFO1A;
	if (v34_phase2_info1a_is_consistent(
		    caller_info0, answerer_info0, info1c, preferred)) {
		*selected = *preferred;
		return V34_PHASE2_SELECTION_OK;
	}

	ceiling = preferred->answer_to_call_symbol_rate <
			  preferred->call_to_answer_symbol_rate ?
			  preferred->answer_to_call_symbol_rate :
			  preferred->call_to_answer_symbol_rate;
	for (unsigned int index = V34_SYMBOL_RATE_COUNT; index-- != 0u;) {
		enum v34_symbol_rate rate = (enum v34_symbol_rate)index;
		uint8_t limit = v34_phase2_projected_rate_limit(rate);

		if (index > ceiling)
			continue;
		if (info1c->answer_transmit[rate]
			    .projected_data_rate_2400_bps_units == 0 ||
		    limit == 0 || !receiver_supports_rate(answerer_info0, rate))
			continue;
		candidate = *preferred;
		candidate.answer_to_call_symbol_rate = rate;
		candidate.call_to_answer_symbol_rate = rate;
		if (!select_call_transmit_carrier(
			    caller_info0, rate,
			    candidate.call_transmit.high_carrier,
			    &candidate.call_transmit.high_carrier))
			continue;
		if (candidate.call_transmit
			    .projected_data_rate_2400_bps_units > limit)
			candidate.call_transmit
				.projected_data_rate_2400_bps_units = limit;
		if (v34_phase2_info1a_is_consistent(
			    caller_info0, answerer_info0, info1c,
			    &candidate)) {
			*selected = candidate;
			return V34_PHASE2_SELECTION_OK;
		}
	}
	return V34_PHASE2_SELECTION_NO_COMMON_PROFILE;
}

enum v34_phase2_selection_result v34_phase2_select_profiles(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *info1a,
	struct v34_phase3_profile *caller_transmit,
	struct v34_phase3_profile *answerer_transmit)
{
	struct v34_phase3_profile selected_caller;
	struct v34_phase3_profile selected_answerer;

	if (caller_transmit == NULL || answerer_transmit == NULL ||
	    caller_transmit == answerer_transmit ||
	    !v34_info0_is_valid(caller_info0) ||
	    !v34_info0_is_valid(answerer_info0) ||
	    !v34_info1c_is_valid(info1c) ||
	    !v34_info1a_is_valid(info1a))
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	if (!v34_phase2_info1c_is_consistent(
		    caller_info0, answerer_info0, info1c))
		return V34_PHASE2_SELECTION_INCONSISTENT_INFO1C;
	if (!v34_phase2_info1a_is_consistent(
		    caller_info0, answerer_info0, info1c, info1a))
		return V34_PHASE2_SELECTION_INCONSISTENT_INFO1A;
	if (v34_phase3_caller_profile(
		    &selected_caller, info1c, info1a) !=
		    V34_PHASE3_PROFILE_OK ||
	    v34_phase3_answerer_profile(
		    &selected_answerer, info1a, info1c) !=
		    V34_PHASE3_PROFILE_OK)
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	*caller_transmit = selected_caller;
	*answerer_transmit = selected_answerer;
	return V34_PHASE2_SELECTION_OK;
}
