// SPDX-License-Identifier: BSD-3-Clause

#include "v34_profile.h"

#include "memory_region.h"
#include "v34_frame_schedule.h"
#include "v34_phase2_selection.h"

#include <stdbool.h>
#include <stddef.h>

enum {
	V34_MAX_RATE_UNITS = V34_MAX_BIT_RATE / V34_RATE_STEP,
	V34_PHASE2_L2_AMPLITUDE = 4096,
};

static bool symbol_rate_from_number(
	unsigned int number, enum v34_symbol_rate *symbol_rate)
{
	switch (number) {
	case 2400:
		*symbol_rate = V34_SYMBOL_RATE_2400;
		return true;
	case 2743:
		*symbol_rate = V34_SYMBOL_RATE_2743;
		return true;
	case 2800:
		*symbol_rate = V34_SYMBOL_RATE_2800;
		return true;
	case 3000:
		*symbol_rate = V34_SYMBOL_RATE_3000;
		return true;
	case 3200:
		*symbol_rate = V34_SYMBOL_RATE_3200;
		return true;
	case 3429:
		*symbol_rate = V34_SYMBOL_RATE_3429;
		return true;
	default:
		return false;
	}
}

static uint16_t requested_rate_mask(
	unsigned int minimum, unsigned int maximum)
{
	uint16_t mask = 0;

	for (uint8_t units = 1; units <= V34_MAX_RATE_UNITS; ++units) {
		unsigned int bit_rate = (unsigned int)units * V34_RATE_STEP;

		if (minimum <= bit_rate && maximum >= bit_rate)
			mask |= (uint16_t)(1u << (units - 1u));
	}
	return mask;
}

static uint8_t highest_rate(uint16_t mask)
{
	for (uint8_t units = V34_MAX_RATE_UNITS; units != 0; --units)
		if ((mask & (uint16_t)(1u << (units - 1u))) != 0)
			return units;
	return 0;
}

static uint16_t enabled_rates(
	unsigned int minimum, unsigned int maximum,
	unsigned int requested_symbol_rate, enum v34_symbol_rate *symbol_rate,
	uint8_t *highest_rate_units)
{
	uint16_t requested = requested_rate_mask(minimum, maximum);
	uint16_t enabled = 0;
	uint8_t highest = highest_rate(requested);
	uint8_t limit;
	enum v34_symbol_rate selected;

	if (highest == 0)
		return 0;
	if (requested_symbol_rate == 0) {
		selected = highest == V34_MIN_BIT_RATE / V34_RATE_STEP ?
				   V34_SYMBOL_RATE_2400 : V34_SYMBOL_RATE_3429;
	} else if (!symbol_rate_from_number(
			   requested_symbol_rate, &selected)) {
		return 0;
	}
	limit = v34_phase2_projected_rate_limit(selected);
	if (limit == 0)
		return 0;
	for (uint8_t units = 1; units <= limit; ++units) {
		struct v34_frame_geometry geometry;

		if ((requested & (uint16_t)(1u << (units - 1u))) != 0 &&
		    v34_frame_geometry_init(
			    &geometry, selected, units, false) ==
			    V34_FRAME_SCHEDULE_OK)
			enabled |= (uint16_t)(1u << (units - 1u));
	}
	*symbol_rate = selected;
	*highest_rate_units = highest_rate(enabled);
	return enabled;
}

static struct v34_info0 local_info0(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 0,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
}

static union v34_local_info1 local_info1(
	enum v34_station_role role, enum v34_symbol_rate symbol_rate,
	uint8_t projected_rate_units, bool fixed_symbol_rate)
{
	union v34_local_info1 info1 = { 0 };

	if (role == V34_CALLING_STATION) {
		info1.calling.common.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT;
		     ++index) {
			uint8_t limit;
			struct v34_info1_probe_result *probe;

			/* A forced caller cadence is expressed by withdrawing every
			 * other probe result before the answerer selects INFO1a. */
			if (fixed_symbol_rate && index != (unsigned int)symbol_rate)
				continue;
			limit = v34_phase2_projected_rate_limit(
				(enum v34_symbol_rate)index);
			probe = &info1.calling.answer_transmit[index];
			probe->high_carrier = index == V34_SYMBOL_RATE_2743;
			probe->projected_data_rate_2400_bps_units =
				projected_rate_units < limit ?
					projected_rate_units : limit;
		}
	} else {
		info1.answering.common.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		info1.answering.call_transmit.high_carrier =
			symbol_rate == V34_SYMBOL_RATE_2743;
		info1.answering.call_transmit
			.projected_data_rate_2400_bps_units = projected_rate_units;
		info1.answering.answer_to_call_symbol_rate = symbol_rate;
		info1.answering.call_to_answer_symbol_rate = symbol_rate;
	}
	return info1;
}

static struct v34_mp_record local_mp(
	uint16_t rate_mask, uint8_t highest_rate_units)
{
	return (struct v34_mp_record){
		.type = V34_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_call_to_answer_rate_2400_bps_units =
					highest_rate_units,
				.maximum_answer_to_call_rate_2400_bps_units =
					highest_rate_units,
				/* Keep the mandatory mode until larger receivers have
				 * comparable interoperability coverage. */
				.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
				.data_rate_capability_mask = rate_mask,
				.asymmetric_rates_enabled = true,
			},
		},
	};
}

enum v34_profile_result v34_profile_init(
	struct v34_call_configuration *configuration,
	uint16_t *enabled_rate_mask, enum v34_station_role role,
	unsigned int minimum_bit_rate, unsigned int maximum_bit_rate,
	unsigned int symbol_rate)
{
	struct v34_call_configuration selected;
	enum v34_symbol_rate selected_symbol_rate;
	uint8_t highest_rate_units = 0;
	uint16_t rate_mask;

	if (configuration == NULL || enabled_rate_mask == NULL ||
	    memory_regions_overlap(
		    configuration, sizeof(*configuration), enabled_rate_mask,
		    sizeof(*enabled_rate_mask)) ||
	    !v34_station_role_is_valid(role) ||
	    minimum_bit_rate > maximum_bit_rate)
		return V34_PROFILE_INVALID_ARGUMENT;
	rate_mask = enabled_rates(
		minimum_bit_rate, maximum_bit_rate, symbol_rate,
		&selected_symbol_rate, &highest_rate_units);
	if (rate_mask == 0)
		return V34_PROFILE_UNSUPPORTED_RATE;
	selected = (struct v34_call_configuration){
		.local_role = role,
		.phase2_nominal_amplitude = V34_PHASE2_L2_AMPLITUDE,
		.local_info0 = local_info0(),
		.local_info1 = local_info1(
			role, selected_symbol_rate, highest_rate_units,
			symbol_rate != 0),
		.phase3 = {
			.requested_remote_constellation =
				V34_CONSTELLATION_4_POINT,
			.sample_rate = V34_PHASE2_SAMPLE_RATE,
			.nominal_peak_amplitude =
				V34_TRAINING_LINE_REFERENCE_AMPLITUDE,
		},
		.phase4 = {
			.transmit_mp = local_mp(rate_mask, highest_rate_units),
		},
	};
	*configuration = selected;
	*enabled_rate_mask = rate_mask;
	return V34_PROFILE_OK;
}
