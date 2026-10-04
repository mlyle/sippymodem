// SPDX-License-Identifier: BSD-3-Clause

#include <v34_profile.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

struct symbol_rate_case {
	unsigned int symbols_per_second;
	enum v34_symbol_rate symbol_rate;
};

static const struct symbol_rate_case symbol_rates[] = {
	{ 2400, V34_SYMBOL_RATE_2400 },
	{ 2743, V34_SYMBOL_RATE_2743 },
	{ 2800, V34_SYMBOL_RATE_2800 },
	{ 3000, V34_SYMBOL_RATE_3000 },
	{ 3200, V34_SYMBOL_RATE_3200 },
	{ 3429, V34_SYMBOL_RATE_3429 },
};

static bool profile_common_is_expected(
	const struct v34_call_configuration *configuration,
	enum v34_station_role role, uint16_t rate_mask, uint8_t highest)
{
	const struct v34_mp_record *record =
		&configuration->phase4.transmit_mp;
	const struct v34_mp_parameters *mp =
		v34_mp_record_common_parameters(record);
	bool zero_precoder = record->type == V34_MP_RECORD_TYPE1;

	for (size_t tap = 0; zero_precoder && tap < V34_PRECODER_TAP_COUNT;
	     ++tap)
		zero_precoder &=
			record->parameters.type1.precoder_coefficients[tap].real == 0 &&
			record->parameters.type1.precoder_coefficients[tap]
				.imaginary == 0;

	return configuration->local_role == role &&
	       configuration->phase2_nominal_amplitude == 4096 &&
	       configuration->local_info0.symbol_rate_2743_supported &&
	       configuration->local_info0.symbol_rate_2800_supported &&
	       configuration->local_info0.symbol_rate_3429_supported &&
	       configuration->local_info0.symbol_rate_3429_transmit_permitted &&
	       configuration->local_info0.maximum_symbol_rate_difference_steps ==
		       0 &&
	       configuration->phase3.requested_remote_constellation ==
		       V34_CONSTELLATION_4_POINT &&
	       configuration->phase3.sample_rate == V34_PHASE2_SAMPLE_RATE &&
	       configuration->phase3.nominal_peak_amplitude == 3792 &&
	       record->type == V34_MP_RECORD_TYPE1 && mp != NULL &&
	       zero_precoder &&
	       mp->maximum_call_to_answer_rate_2400_bps_units == highest &&
	       mp->maximum_answer_to_call_rate_2400_bps_units == highest &&
	       mp->remote_transmitter_trellis == V34_TRELLIS_16_STATE &&
	       mp->data_rate_capability_mask == rate_mask;
}

static bool test_fixed_symbol_rates(void)
{
	for (size_t rate = 0; rate < ARRAY_LENGTH(symbol_rates); ++rate) {
		uint8_t maximum = v34_phase2_projected_rate_limit(
			symbol_rates[rate].symbol_rate);
		unsigned int bit_rate = (unsigned int)maximum * V34_RATE_STEP;

		for (unsigned int role_index = V34_CALLING_STATION;
		     role_index <= V34_ANSWERING_STATION; ++role_index) {
			enum v34_station_role role =
				(enum v34_station_role)role_index;
			struct v34_call_configuration configuration;
			uint16_t rate_mask = 0;

			CHECK(v34_profile_init(
				      &configuration, &rate_mask, role, bit_rate,
				      bit_rate,
				      symbol_rates[rate].symbols_per_second) ==
				      V34_PROFILE_OK &&
			      rate_mask ==
				      (uint16_t)(1u << (maximum - 1u)) &&
			      profile_common_is_expected(
				      &configuration, role, rate_mask, maximum),
			      "role %u rate %u profile did not initialize",
			      role_index, symbol_rates[rate].symbols_per_second);
			if (role == V34_CALLING_STATION) {
				for (unsigned int candidate = 0;
				     candidate < V34_SYMBOL_RATE_COUNT; ++candidate)
					CHECK(configuration.local_info1.calling
						      .answer_transmit[candidate]
						      .projected_data_rate_2400_bps_units ==
					      (candidate == rate ? maximum : 0),
					      "fixed caller rate %zu advertised candidate %u",
					      rate, candidate);
			} else {
				CHECK(configuration.local_info1.answering
					      .answer_to_call_symbol_rate ==
					      symbol_rates[rate].symbol_rate &&
				      configuration.local_info1.answering
					      .call_to_answer_symbol_rate ==
					      symbol_rates[rate].symbol_rate &&
				      configuration.local_info1.answering
					      .call_transmit
					      .projected_data_rate_2400_bps_units ==
					      maximum,
				      "fixed answerer rate %zu changed", rate);
			}
		}
	}
	return true;
}

static bool test_caller_forces_remote_selection(void)
{
	struct v34_call_configuration caller;
	struct v34_call_configuration answerer;
	struct v34_info1a selected;
	uint16_t caller_mask;
	uint16_t answerer_mask;

	CHECK(v34_profile_init(
		      &caller, &caller_mask, V34_CALLING_STATION, 4800, 4800,
		      2400) == V34_PROFILE_OK &&
	      v34_profile_init(
		      &answerer, &answerer_mask, V34_ANSWERING_STATION, 4800,
		      4800, 3429) == V34_PROFILE_OK,
	      "forced selection profiles did not initialize");
	CHECK(v34_phase2_select_answerer_info1a(
		      &caller.local_info0, &answerer.local_info0,
		      &caller.local_info1.calling,
		      &answerer.local_info1.answering, &selected) ==
		      V34_PHASE2_SELECTION_OK &&
	      selected.answer_to_call_symbol_rate == V34_SYMBOL_RATE_2400 &&
	      selected.call_to_answer_symbol_rate == V34_SYMBOL_RATE_2400,
	      "remote preference escaped the caller's forced cadence");
	return true;
}

static bool test_automatic_profile(void)
{
	struct v34_call_configuration caller;
	struct v34_call_configuration answerer;
	uint16_t caller_mask;
	uint16_t answerer_mask;

	CHECK(v34_profile_init(
		      &caller, &caller_mask, V34_CALLING_STATION,
		      V34_MIN_BIT_RATE, V34_MAX_BIT_RATE, 0) == V34_PROFILE_OK &&
	      v34_profile_init(
		      &answerer, &answerer_mask, V34_ANSWERING_STATION,
		      V34_MIN_BIT_RATE, V34_MAX_BIT_RATE, 0) == V34_PROFILE_OK,
	      "automatic profiles did not initialize");
	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate)
		CHECK(caller.local_info1.calling.answer_transmit[rate]
			      .projected_data_rate_2400_bps_units ==
			      v34_phase2_projected_rate_limit(
				      (enum v34_symbol_rate)rate),
		      "automatic caller withdrew rate %u", rate);
	CHECK(answerer.local_info1.answering.answer_to_call_symbol_rate ==
		      V34_SYMBOL_RATE_3429 &&
	      answerer.local_info1.answering.call_to_answer_symbol_rate ==
		      V34_SYMBOL_RATE_3429 &&
	      caller_mask == answerer_mask,
	      "automatic answerer did not prefer the highest cadence");
	return true;
}

static bool test_validation(void)
{
	struct v34_call_configuration configuration;
	struct v34_call_configuration saved;
	uint16_t rate_mask = UINT16_C(0xa55a);
	uint16_t saved_mask;

	memset(&configuration, 0xa5, sizeof(configuration));
	saved = configuration;
	saved_mask = rate_mask;
	CHECK(v34_profile_init(
		      NULL, &rate_mask, V34_CALLING_STATION, 4800, 4800,
		      2400) == V34_PROFILE_INVALID_ARGUMENT &&
	      v34_profile_init(
		      &configuration, NULL, V34_CALLING_STATION, 4800, 4800,
		      2400) == V34_PROFILE_INVALID_ARGUMENT &&
	      v34_profile_init(
		      &configuration, (uint16_t *)&configuration,
		      V34_CALLING_STATION, 4800, 4800,
		      2400) == V34_PROFILE_INVALID_ARGUMENT &&
	      v34_profile_init(
		      &configuration, &rate_mask,
		      (enum v34_station_role)2, 4800, 4800,
		      2400) == V34_PROFILE_INVALID_ARGUMENT &&
	      v34_profile_init(
		      &configuration, &rate_mask, V34_CALLING_STATION, 9600,
		      4800, 2400) == V34_PROFILE_INVALID_ARGUMENT &&
	      v34_profile_init(
		      &configuration, &rate_mask, V34_CALLING_STATION, 4800,
		      4800, 2600) == V34_PROFILE_UNSUPPORTED_RATE &&
	      v34_profile_init(
		      &configuration, &rate_mask, V34_CALLING_STATION, 33600,
		      33600, 2400) == V34_PROFILE_UNSUPPORTED_RATE &&
	      memcmp(&configuration, &saved, sizeof(configuration)) == 0 &&
	      rate_mask == saved_mask,
	      "invalid profile changed an output");
	return true;
}

static bool test_independent_direction_rates(void)
{
	struct v34_call_configuration caller;
	struct v34_call_configuration answerer;
	struct v34_mp_rates rates;
	uint16_t caller_mask, answerer_mask;

	CHECK(v34_profile_init(&caller, &caller_mask, V34_CALLING_STATION,
		      V34_MIN_BIT_RATE, V34_MAX_BIT_RATE, 0) == V34_PROFILE_OK &&
	      v34_profile_init(&answerer, &answerer_mask, V34_ANSWERING_STATION,
		      V34_MIN_BIT_RATE, V34_MAX_BIT_RATE, 0) == V34_PROFILE_OK,
	      "directional profiles did not initialize");
	/* Each receiver limits only its own direction after training. */
	caller.phase4.transmit_mp.parameters.type1.common
		.maximum_answer_to_call_rate_2400_bps_units = 8;
	answerer.phase4.transmit_mp.parameters.type1.common
		.maximum_call_to_answer_rate_2400_bps_units = 9;
	CHECK(v34_mp_select_rates(&caller.phase4.transmit_mp,
		      &answerer.phase4.transmit_mp, &rates) == V34_MP_RATE_OK &&
	      rates.asymmetric_rates_enabled &&
	      rates.call_to_answer_2400_bps_units == 9 &&
	      rates.answer_to_call_2400_bps_units == 8,
	      "one receiver's ceiling limited both directions");
	answerer.phase4.transmit_mp.parameters.type1.common
		.asymmetric_rates_enabled = false;
	CHECK(v34_mp_select_rates(&caller.phase4.transmit_mp,
		      &answerer.phase4.transmit_mp, &rates) == V34_MP_RATE_OK &&
	      !rates.asymmetric_rates_enabled &&
	      rates.call_to_answer_2400_bps_units == 8 &&
	      rates.answer_to_call_2400_bps_units == 8,
	      "symmetric-only peer did not retain a common rate");
	return true;
}

static bool test_peer_power_reduction(void)
{
	struct v34_call_configuration caller;
	struct v34_call_configuration answerer;
	struct v34_phase3_profile transmit;
	uint16_t mask;

	CHECK(v34_profile_init(&caller, &mask, V34_CALLING_STATION,
		      V34_MIN_BIT_RATE, V34_MAX_BIT_RATE, 3200) == V34_PROFILE_OK &&
	      v34_profile_init(&answerer, &mask, V34_ANSWERING_STATION,
		      V34_MIN_BIT_RATE, V34_MAX_BIT_RATE, 3200) == V34_PROFILE_OK,
	      "power reduction profiles did not initialize");
	for (uint8_t reduction = 1;
	     reduction <= V34_INFO1_MAX_POWER_REDUCTION_DB; ++reduction) {
		caller.local_info1.calling.common.minimum_peer_power_reduction_db =
			reduction;
		answerer.local_info1.answering.common.minimum_peer_power_reduction_db =
			reduction;
		CHECK(v34_phase2_info1a_is_consistent(
			      &caller.local_info0, &answerer.local_info0,
			      &caller.local_info1.calling,
			      &answerer.local_info1.answering),
		      "profile rejected a supported %u dB peer request", reduction);
		CHECK(v34_phase3_caller_profile(&transmit,
			      &caller.local_info1.calling,
			      &answerer.local_info1.answering) == V34_PHASE3_PROFILE_OK &&
		      transmit.transmit_power_reduction_db == reduction,
		      "caller lost the peer's power reduction");
		CHECK(v34_phase3_answerer_profile(&transmit,
			      &answerer.local_info1.answering,
			      &caller.local_info1.calling) == V34_PHASE3_PROFILE_OK &&
		      transmit.transmit_power_reduction_db == reduction,
		      "answerer lost the peer's power reduction");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "fixed symbol rates", test_fixed_symbol_rates },
		{ "caller forced selection", test_caller_forces_remote_selection },
		{ "automatic profile", test_automatic_profile },
		{ "independent direction rates", test_independent_direction_rates },
		{ "peer power reduction", test_peer_power_reduction },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
