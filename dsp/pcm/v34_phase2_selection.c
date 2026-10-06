// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_selection.h"

#include "memory_region.h"
#include "phase2_ranging.h"

#include <math.h>
#include <stddef.h>

enum {
	V34_PREEMPHASIS_THRESHOLD_BIN = 4,
	V34_PREEMPHASIS_2400_2800_BIN = 18,
	V34_PREEMPHASIS_3000_BIN = 19,
	V34_PREEMPHASIS_3200_BIN = 20,
	V34_PREEMPHASIS_3429_BIN = 22,
	V34_PREEMPHASIS_FIRST_FILTER = 6,
	V90_PROBE_METRIC_LOW_PRIMARY = 2,
	V90_PROBE_METRIC_2800 = 20,
	V90_PROBE_METRIC_3000 = 22,
	V90_PROBE_METRIC_3200 = 23,
	V90_PROBE_METRIC_MAXIMUM = 20,
	V90_INFO1D_ADDITIONAL_POWER_REDUCTION_DB = 3,
};

bool v34_phase2_handoff_is_valid(
	const struct v34_phase2_handoff *handoff)
{
	struct v34_phase3_profile caller;
	struct v34_phase3_profile answerer;

	return v34_phase2_handoff_profiles(
		       handoff, &caller, &answerer) ==
	       V34_PHASE2_SELECTION_OK;
}

enum v34_phase2_selection_result v34_phase2_handoff_profiles(
	const struct v34_phase2_handoff *handoff,
	struct v34_phase3_profile *caller_transmit,
	struct v34_phase3_profile *answerer_transmit)
{
	if (handoff == NULL || caller_transmit == NULL ||
	    answerer_transmit == NULL ||
	    memory_regions_overlap(
		    handoff, sizeof(*handoff), caller_transmit,
		    sizeof(*caller_transmit)) ||
	    memory_regions_overlap(
		    handoff, sizeof(*handoff), answerer_transmit,
		    sizeof(*answerer_transmit)) ||
	    !v34_station_role_is_valid(handoff->local_role) ||
	    !v34_info0_is_valid(&handoff->local_info0) ||
	    !v34_info0_is_valid(&handoff->peer_info0) ||
	    handoff->receive_gain <= 0 ||
	    handoff->round_trip_delay_samples >
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			    PHASE2_RANGING_TURNAROUND_SAMPLES)
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	return v34_phase2_select_profiles(
		handoff->local_role == V34_CALLING_STATION ?
			&handoff->local_info0 : &handoff->peer_info0,
		handoff->local_role == V34_ANSWERING_STATION ?
			&handoff->local_info0 : &handoff->peer_info0,
		&handoff->caller_info1c, &handoff->answerer_info1a,
		caller_transmit, answerer_transmit);
}

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
	    !v34_phase2_probe_measurement_is_valid(measurement))
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	sample = measurement->l2.harmonics[profile.source_bin].mean_square;
	threshold = measurement->l2
			    .harmonics[V34_PREEMPHASIS_THRESHOLD_BIN]
			    .mean_square;
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

static bool v90_probe_bin_is_active(size_t index)
{
	return index < V34_PHASE2_PROBE_HIGHEST_HARMONIC &&
	       index != 5u && index != 7u && index != 11u && index != 15u;
}

static bool v90_probe_metrics(
	const struct v34_phase2_probe_receiver_measurement *measurement,
	uint8_t metrics[V34_PHASE2_PROBE_HIGHEST_HARMONIC])
{
	double maximum = 0.0;
	double maximum_fraction;
	int maximum_exponent;

	if (!v34_phase2_probe_measurement_is_valid(measurement))
		return false;
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index) {
		double power = measurement->l2.harmonics[index].mean_square;

		if (v90_probe_bin_is_active(index) && power > maximum)
			maximum = power;
	}
	if (maximum <= 0.0 || !isfinite(maximum))
		return false;
	maximum_fraction = frexp(maximum, &maximum_exponent);
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index) {
		double power = measurement->l2.harmonics[index].mean_square;
		double fraction;
		int exponent;
		int attenuation;

		if (!v90_probe_bin_is_active(index) || power == 0.0) {
			metrics[index] = V90_PROBE_METRIC_MAXIMUM;
			continue;
		}
		fraction = frexp(power, &exponent);
		attenuation = maximum_exponent - exponent;
		if (maximum_fraction < fraction)
			attenuation--;
		if (attenuation < 0)
			attenuation = 0;
		if (attenuation > V90_PROBE_METRIC_MAXIMUM)
			attenuation = V90_PROBE_METRIC_MAXIMUM;
		metrics[index] = (uint8_t)attenuation;
	}

	/* Compensate the known probe edge geometry before applying the V.90
	 * carrier and projection thresholds. */
	{
		unsigned int low = metrics[2] + metrics[3] + metrics[4] +
			metrics[6];
		unsigned int high = metrics[12] + metrics[13] + metrics[14] +
			metrics[16];
		uint8_t reduction = 0;

		if (low == 0u)
			low = 1u;
		if (high > low * 4u)
			reduction = 2;
		else if (high > low * 2u)
			reduction = 1;
		if (reduction != 0u) {
			for (size_t index = 16; index < 24; ++index) {
				if (metrics[index] > reduction)
					metrics[index] -= reduction;
			}
		}
	}
	return true;
}

static uint8_t v90_projected_rate(
	enum v34_symbol_rate rate, const uint8_t *metrics)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		return 9;
	case V34_SYMBOL_RATE_2743:
		return 0;
	case V34_SYMBOL_RATE_2800:
		return 11;
	case V34_SYMBOL_RATE_3000:
		if (metrics[V90_PROBE_METRIC_3000] < 7)
			return 12;
		return metrics[V90_PROBE_METRIC_3000] < 8 ? 10 : 9;
	case V34_SYMBOL_RATE_3200:
		if (metrics[V90_PROBE_METRIC_3200] == 0)
			return 13;
		return metrics[V90_PROBE_METRIC_3200] < 8 ? 12 : 11;
	case V34_SYMBOL_RATE_3429:
		return 14;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return 0;
}

static bool v90_high_carrier(
	enum v34_symbol_rate rate, const uint8_t *metrics)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2800:
		return metrics[V90_PROBE_METRIC_2800] < 6 &&
		       metrics[V90_PROBE_METRIC_LOW_PRIMARY] < 6;
	case V34_SYMBOL_RATE_3000:
		return metrics[V90_PROBE_METRIC_3000] < 6 &&
		       metrics[V90_PROBE_METRIC_LOW_PRIMARY] < 6;
	case V34_SYMBOL_RATE_3200:
		return metrics[V90_PROBE_METRIC_3200] < 6;
	case V34_SYMBOL_RATE_2400:
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_3429:
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static bool symbol_rate_is_supported(
	const struct v34_info0 *info0, enum v34_symbol_rate rate)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
	case V34_SYMBOL_RATE_3000:
	case V34_SYMBOL_RATE_3200:
		return true;
	case V34_SYMBOL_RATE_2743:
		return info0->symbol_rate_2743_supported;
	case V34_SYMBOL_RATE_2800:
		return info0->symbol_rate_2800_supported;
	case V34_SYMBOL_RATE_3429:
		return info0->symbol_rate_3429_supported;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static bool transmitter_supports_rate(
	const struct v34_info0 *transmitter, enum v34_symbol_rate rate,
	bool high_carrier)
{
	return symbol_rate_is_supported(transmitter, rate) &&
	       (rate != V34_SYMBOL_RATE_3000 ||
	        (high_carrier ? transmitter->high_carrier_3000_supported :
	                        transmitter->low_carrier_3000_supported)) &&
	       (rate != V34_SYMBOL_RATE_3200 ||
	        (high_carrier ? transmitter->high_carrier_3200_supported :
	                        transmitter->low_carrier_3200_supported)) &&
	       (rate != V34_SYMBOL_RATE_3429 ||
	        transmitter->symbol_rate_3429_transmit_permitted);
}

static bool select_transmit_carrier(
	const struct v34_info0 *transmitter, enum v34_symbol_rate rate,
	bool preferred, bool *selected)
{
	if (transmitter_supports_rate(transmitter, rate, preferred)) {
		*selected = preferred;
		return true;
	}
	if (transmitter_supports_rate(transmitter, rate, !preferred)) {
		*selected = !preferred;
		return true;
	}
	return false;
}

enum v34_phase2_selection_result v34_phase2_select_v90_info1d(
	const struct v34_phase2_probe_receiver_measurement *measurement,
	const struct v34_info1c *configured,
	const struct v34_info0 *peer,
	struct v34_info1c *selected)
{
	uint8_t metrics[V34_PHASE2_PROBE_HIGHEST_HARMONIC];
	struct v34_info1c result = { 0 };

	if (measurement == NULL || configured == NULL || peer == NULL ||
	    selected == NULL ||
	    memory_regions_overlap(
		    selected, sizeof(*selected), measurement,
		    sizeof(*measurement)) ||
	    memory_regions_overlap(
		    selected, sizeof(*selected), configured,
		    sizeof(*configured)) ||
	    memory_regions_overlap(selected, sizeof(*selected), peer, sizeof(*peer)))
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	if (!v34_info1c_is_valid(configured) || !v34_info0_is_valid(peer) ||
	    !v90_probe_metrics(measurement, metrics))
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	result.common = configured->common;
	result.common.minimum_peer_power_reduction_db = 0;
	result.common.additional_peer_power_reduction_db =
		peer->transmit_power_reduction_supported ?
			V90_INFO1D_ADDITIONAL_POWER_REDUCTION_DB : 0;
	for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index) {
		enum v34_symbol_rate rate = (enum v34_symbol_rate)index;
		uint8_t projected = v90_projected_rate(rate, metrics);
		struct v34_info1_probe_result *probe =
			&result.answer_transmit[index];

		if (configured->answer_transmit[index]
			    .projected_data_rate_2400_bps_units == 0 || projected == 0 ||
		    !select_transmit_carrier(
			    peer, rate, v90_high_carrier(rate, metrics),
			    &probe->high_carrier))
			continue;
		probe->projected_data_rate_2400_bps_units =
			!peer->constellation_1664_supported && projected > 12 ?
				12 : projected;
		if (v34_phase2_select_preemphasis(
			    measurement, rate,
			    &probe->preemphasis_filter_index) !=
		    V34_PHASE2_SELECTION_OK)
			return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	}
	if (!v34_info1c_is_valid(&result))
		return V34_PHASE2_SELECTION_INVALID_ARGUMENT;
	*selected = result;
	return V34_PHASE2_SELECTION_OK;
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
	       symbol_rate_is_supported(receiver, rate) &&
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
		    limit == 0 || !symbol_rate_is_supported(answerer_info0, rate))
			continue;
		candidate = *preferred;
		candidate.answer_to_call_symbol_rate = rate;
		candidate.call_to_answer_symbol_rate = rate;
		if (!select_transmit_carrier(
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
	    memory_regions_overlap(
		    caller_transmit, sizeof(*caller_transmit),
		    answerer_transmit, sizeof(*answerer_transmit)) ||
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
