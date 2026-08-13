// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_profile.h"

#include <stddef.h>

struct exact_fraction {
	uint32_t numerator;
	uint32_t denominator;
};

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static bool symbol_rate_fraction(
	enum v34_symbol_rate rate, struct exact_fraction *fraction)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		*fraction = (struct exact_fraction){ 2400, 1 };
		return true;
	case V34_SYMBOL_RATE_2743:
		*fraction = (struct exact_fraction){ 19200, 7 };
		return true;
	case V34_SYMBOL_RATE_2800:
		*fraction = (struct exact_fraction){ 2800, 1 };
		return true;
	case V34_SYMBOL_RATE_3000:
		*fraction = (struct exact_fraction){ 3000, 1 };
		return true;
	case V34_SYMBOL_RATE_3200:
		*fraction = (struct exact_fraction){ 3200, 1 };
		return true;
	case V34_SYMBOL_RATE_3429:
		*fraction = (struct exact_fraction){ 24000, 7 };
		return true;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static bool carrier_ratio(
	enum v34_symbol_rate rate, bool high_carrier,
	struct exact_fraction *ratio)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		*ratio = high_carrier ?
			(struct exact_fraction){ 3, 4 } :
			(struct exact_fraction){ 2, 3 };
		return true;
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_2800:
	case V34_SYMBOL_RATE_3000:
		*ratio = high_carrier ?
			(struct exact_fraction){ 2, 3 } :
			(struct exact_fraction){ 3, 5 };
		return true;
	case V34_SYMBOL_RATE_3200:
		*ratio = high_carrier ?
			(struct exact_fraction){ 3, 5 } :
			(struct exact_fraction){ 4, 7 };
		return true;
	case V34_SYMBOL_RATE_3429:
		*ratio = (struct exact_fraction){ 4, 7 };
		return true;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static uint32_t greatest_common_divisor(uint32_t left, uint32_t right)
{
	while (right != 0) {
		uint32_t remainder = left % right;

		left = right;
		right = remainder;
	}
	return left;
}

static struct exact_fraction carrier_frequency(
	struct exact_fraction symbol_rate, struct exact_fraction ratio)
{
	struct exact_fraction frequency = {
		.numerator = symbol_rate.numerator * ratio.numerator,
		.denominator = symbol_rate.denominator * ratio.denominator,
	};
	uint32_t divisor = greatest_common_divisor(
		frequency.numerator, frequency.denominator);

	frequency.numerator /= divisor;
	frequency.denominator /= divisor;
	return frequency;
}

static bool md_symbol_count(
	struct exact_fraction symbol_rate, uint8_t duration_units,
	uint32_t *symbol_count)
{
	uint64_t numerator = (uint64_t)symbol_rate.numerator * 35u *
			     duration_units;
	uint64_t denominator = (uint64_t)symbol_rate.denominator * 1000u;

	if (numerator % denominator != 0)
		return false;
	*symbol_count = (uint32_t)(numerator / denominator);
	return true;
}

static bool profile_fields_match_rates(
	const struct v34_phase3_profile *profile)
{
	struct exact_fraction symbol_rate;
	struct exact_fraction ratio;
	struct exact_fraction frequency;
	uint32_t expected_md_symbols;

	if (!symbol_rate_fraction(profile->symbol_rate, &symbol_rate) ||
	    !carrier_ratio(profile->symbol_rate, profile->high_carrier, &ratio) ||
	    !md_symbol_count(
		    symbol_rate, profile->md_length_35_ms_units,
		    &expected_md_symbols))
		return false;
	frequency = carrier_frequency(symbol_rate, ratio);
	return profile->symbol_rate_numerator == symbol_rate.numerator &&
	       profile->symbol_rate_denominator == symbol_rate.denominator &&
	       profile->carrier_frequency_numerator == frequency.numerator &&
	       profile->carrier_frequency_denominator == frequency.denominator &&
	       profile->md_symbol_count == expected_md_symbols;
}

bool v34_phase3_profile_is_valid(
	const struct v34_phase3_profile *profile)
{
	return profile != NULL && role_is_valid(profile->local_role) &&
	       profile->transmit_power_reduction_db <=
		       V34_INFO1_MAX_POWER_REDUCTION_DB &&
	       profile->preemphasis_filter_index <=
		       V34_INFO1_MAX_PREEMPHASIS_FILTER_INDEX &&
	       profile->projected_data_rate_2400_bps_units > 0 &&
	       profile->projected_data_rate_2400_bps_units <=
		       V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS &&
	       profile->md_length_35_ms_units <=
		       V34_INFO1_MAX_MD_LENGTH_35_MS_UNITS &&
	       profile_fields_match_rates(profile);
}

enum v34_phase3_profile_result v34_phase3_profile_init(
	struct v34_phase3_profile *profile,
	enum v34_station_role local_role,
	enum v34_symbol_rate symbol_rate,
	const struct v34_info1_probe_result *probe_result,
	uint8_t power_reduction_db, uint8_t md_length_35_ms_units)
{
	struct v34_phase3_profile selected;
	struct exact_fraction rate;
	struct exact_fraction ratio;
	struct exact_fraction frequency;

	if (profile == NULL || probe_result == NULL ||
	    !role_is_valid(local_role) ||
	    power_reduction_db > V34_INFO1_MAX_POWER_REDUCTION_DB ||
	    probe_result->preemphasis_filter_index >
		    V34_INFO1_MAX_PREEMPHASIS_FILTER_INDEX ||
	    probe_result->projected_data_rate_2400_bps_units >
		    V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS ||
	    md_length_35_ms_units > V34_INFO1_MAX_MD_LENGTH_35_MS_UNITS)
		return V34_PHASE3_PROFILE_INVALID_ARGUMENT;
	selected = (struct v34_phase3_profile){
		.symbol_rate = symbol_rate,
		.local_role = local_role,
		.transmit_power_reduction_db = power_reduction_db,
		.preemphasis_filter_index =
			probe_result->preemphasis_filter_index,
		.projected_data_rate_2400_bps_units =
			probe_result->projected_data_rate_2400_bps_units,
		.md_length_35_ms_units = md_length_35_ms_units,
		.high_carrier = probe_result->high_carrier,
	};

	if (probe_result->projected_data_rate_2400_bps_units == 0)
		return V34_PHASE3_PROFILE_UNUSABLE_SELECTION;
	if (!symbol_rate_fraction(symbol_rate, &rate) ||
	    !carrier_ratio(symbol_rate, selected.high_carrier, &ratio) ||
	    !md_symbol_count(
		    rate, md_length_35_ms_units, &selected.md_symbol_count))
		return V34_PHASE3_PROFILE_INVALID_ARGUMENT;
	frequency = carrier_frequency(rate, ratio);
	selected.symbol_rate_numerator = rate.numerator;
	selected.symbol_rate_denominator = rate.denominator;
	selected.carrier_frequency_numerator = frequency.numerator;
	selected.carrier_frequency_denominator = frequency.denominator;
	if (!v34_phase3_profile_is_valid(&selected))
		return V34_PHASE3_PROFILE_INVALID_ARGUMENT;
	*profile = selected;
	return V34_PHASE3_PROFILE_OK;
}

enum v34_phase3_profile_result
v34_phase3_caller_profile(
	struct v34_phase3_profile *profile,
	const struct v34_info1c *local_info1c,
	const struct v34_info1a *peer_info1a)
{
	if (profile == NULL || !v34_info1c_is_valid(local_info1c) ||
	    !v34_info1a_is_valid(peer_info1a))
		return V34_PHASE3_PROFILE_INVALID_ARGUMENT;
	return v34_phase3_profile_init(
		profile, V34_CALLING_STATION,
		peer_info1a->call_to_answer_symbol_rate,
		&peer_info1a->call_transmit,
		peer_info1a->common.minimum_peer_power_reduction_db,
		local_info1c->common.local_md_length_35_ms_units);
}

enum v34_phase3_profile_result
v34_phase3_answerer_profile(
	struct v34_phase3_profile *profile,
	const struct v34_info1a *local_info1a,
	const struct v34_info1c *peer_info1c)
{
	enum v34_symbol_rate symbol_rate;

	if (profile == NULL || !v34_info1a_is_valid(local_info1a) ||
	    !v34_info1c_is_valid(peer_info1c))
		return V34_PHASE3_PROFILE_INVALID_ARGUMENT;
	symbol_rate = local_info1a->answer_to_call_symbol_rate;
	return v34_phase3_profile_init(
		profile, V34_ANSWERING_STATION, symbol_rate,
		&peer_info1c->answer_transmit[symbol_rate],
		peer_info1c->common.minimum_peer_power_reduction_db,
		local_info1a->common.local_md_length_35_ms_units);
}

const char *v34_phase3_profile_result_name(
	enum v34_phase3_profile_result result)
{
	switch (result) {
	case V34_PHASE3_PROFILE_OK:
		return "ok";
	case V34_PHASE3_PROFILE_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_PROFILE_UNUSABLE_SELECTION:
		return "unusable selection";
	}
	return "unknown";
}
