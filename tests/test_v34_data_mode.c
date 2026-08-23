// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_mode.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static struct v34_mp_parameters calling_parameters(void)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 9,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis = V34_TRELLIS_64_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.remote_transmitter_expanded_shaping = false,
		.data_rate_capability_mask = 0x07ff,
		.asymmetric_rates_enabled = true,
	};
}

static struct v34_mp_parameters answering_parameters(void)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 8,
		.maximum_answer_to_call_rate_2400_bps_units = 11,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
		.remote_transmitter_nonlinear_encoding = false,
		.remote_transmitter_expanded_shaping = true,
		.data_rate_capability_mask = 0x07ff,
		.asymmetric_rates_enabled = true,
	};
}

static struct v34_mp_record calling_record(void)
{
	return (struct v34_mp_record){
		.type = V34_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = calling_parameters(),
			.precoder_coefficients = {
				{ 4096, -2048 },
				{ 1024, 3072 },
				{ -1536, 512 },
			},
		},
	};
}

static struct v34_mp_record answering_record(void)
{
	return (struct v34_mp_record){
		.type = V34_MP_RECORD_TYPE0,
		.parameters.type0 = answering_parameters(),
	};
}

static bool coefficients_are_zero(
	const struct v34_data_direction_parameters *direction)
{
	for (size_t index = 0; index < V34_PRECODER_TAP_COUNT; ++index) {
		if (direction->precoder_coefficients[index].real != 0 ||
		    direction->precoder_coefficients[index].imaginary != 0)
			return false;
	}
	return true;
}

static bool directions_are_equal(
	const struct v34_data_direction_parameters *left,
	const struct v34_data_direction_parameters *right)
{
	return left->trellis_selection == right->trellis_selection &&
	       left->nonlinear_parameter == right->nonlinear_parameter &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->expanded_shaping == right->expanded_shaping &&
	       memcmp(left->precoder_coefficients,
		      right->precoder_coefficients,
		      sizeof(left->precoder_coefficients)) == 0;
}

static bool test_complementary_startup_modes(void)
{
	struct v34_mp_record calling = calling_record();
	struct v34_mp_record answering = answering_record();
	struct v34_data_mode caller;
	struct v34_data_mode answerer;

	CHECK(v34_data_mode_select_startup(
		      V34_CALLING_STATION, &calling, &answering,
		      &caller) == V34_DATA_MODE_OK &&
	      v34_data_mode_select_startup(
		      V34_ANSWERING_STATION, &answering, &calling,
		      &answerer) == V34_DATA_MODE_OK &&
	      v34_data_mode_is_valid(&caller) &&
	      v34_data_mode_is_valid(&answerer),
	      "valid MP records did not select startup modes");
	CHECK(caller.transmit.primary_data_rate_2400_bps_units == 8 &&
	      caller.receive.primary_data_rate_2400_bps_units == 9 &&
	      caller.transmit.trellis_selection == V34_TRELLIS_16_STATE &&
	      caller.transmit.nonlinear_parameter ==
		      V34_NONLINEAR_THETA_ZERO &&
	      caller.transmit.expanded_shaping &&
	      coefficients_are_zero(&caller.transmit),
	      "caller transmission did not follow the peer receiver request");
	CHECK(caller.receive.trellis_selection == V34_TRELLIS_64_STATE &&
	      caller.receive.nonlinear_parameter ==
		      V34_NONLINEAR_THETA_5_OVER_16 &&
	      !caller.receive.expanded_shaping &&
	      memcmp(caller.receive.precoder_coefficients,
		     calling.parameters.type1.precoder_coefficients,
		     sizeof(caller.receive.precoder_coefficients)) == 0,
	      "caller reception did not retain its own remote request");
	CHECK(directions_are_equal(&caller.transmit, &answerer.receive) &&
	      directions_are_equal(&caller.receive, &answerer.transmit) &&
	      caller.auxiliary_channel_enabled &&
	      answerer.auxiliary_channel_enabled &&
	      caller.asymmetric_rates_enabled &&
	      answerer.asymmetric_rates_enabled,
	      "the two stations selected incompatible directional modes");
	return true;
}

static bool test_symmetric_selection(void)
{
	struct v34_mp_record calling = calling_record();
	struct v34_mp_record answering = answering_record();
	struct v34_data_mode selected;

	answering.parameters.type0.asymmetric_rates_enabled = false;
	answering.parameters.type0.auxiliary_channel_enabled = false;
	answering.parameters.type0.data_rate_capability_mask &=
		(uint16_t)~(1u << (8u - 1u));
	CHECK(v34_data_mode_select_startup(
		      V34_CALLING_STATION, &calling, &answering,
		      &selected) == V34_DATA_MODE_OK &&
	      selected.transmit.primary_data_rate_2400_bps_units == 7 &&
	      selected.receive.primary_data_rate_2400_bps_units == 7 &&
	      !selected.auxiliary_channel_enabled &&
	      !selected.asymmetric_rates_enabled,
	      "startup mode did not retain symmetric rate selection");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_mp_record calling = calling_record();
	struct v34_mp_record answering = answering_record();
	struct v34_data_mode selected;
	struct v34_data_mode invalid;
	uint8_t saved[sizeof(selected)];

	memset(&selected, 0xa5, sizeof(selected));
	memcpy(saved, &selected, sizeof(saved));
	calling.parameters.type1.common.data_rate_capability_mask = UINT16_C(1);
	answering.parameters.type0.data_rate_capability_mask = UINT16_C(2);
	CHECK(v34_data_mode_select_startup(
		      V34_CALLING_STATION, &calling, &answering,
		      &selected) == V34_DATA_MODE_NO_COMMON_RATE &&
	      memcmp(&selected, saved, sizeof(selected)) == 0,
	      "a capability gap changed the destination");
	CHECK(v34_data_mode_select_startup(
		      (enum v34_station_role)99, &calling, &answering,
		      &selected) == V34_DATA_MODE_INVALID_ARGUMENT &&
	      v34_data_mode_select_startup(
		      V34_CALLING_STATION, NULL, &answering,
		      &selected) == V34_DATA_MODE_INVALID_ARGUMENT &&
	      v34_data_mode_select_startup(
		      V34_CALLING_STATION, &calling, NULL,
		      &selected) == V34_DATA_MODE_INVALID_ARGUMENT &&
	      v34_data_mode_select_startup(
		      V34_CALLING_STATION, &calling, &answering,
		      NULL) == V34_DATA_MODE_INVALID_ARGUMENT &&
	      memcmp(&selected, saved, sizeof(selected)) == 0,
	      "invalid startup selection changed the destination");

	invalid = (struct v34_data_mode){
		.transmit = {
			.trellis_selection = V34_TRELLIS_16_STATE,
			.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO,
			.primary_data_rate_2400_bps_units = 4,
		},
		.receive = {
			.trellis_selection = V34_TRELLIS_32_STATE,
			.nonlinear_parameter = V34_NONLINEAR_THETA_5_OVER_16,
			.primary_data_rate_2400_bps_units = 4,
		},
	};
	CHECK(v34_data_mode_is_valid(&invalid),
	      "valid zero-precoder mode was rejected");
	invalid.transmit.primary_data_rate_2400_bps_units = 0;
	CHECK(!v34_data_mode_is_valid(NULL) &&
	      !v34_data_mode_is_valid(&invalid),
	      "invalid primary rate was accepted");
	invalid.transmit.primary_data_rate_2400_bps_units = 4;
	invalid.transmit.trellis_selection =
		(enum v34_trellis_selection)99;
	CHECK(!v34_data_mode_is_valid(&invalid),
	      "invalid trellis selection was accepted");
	invalid.transmit.trellis_selection = V34_TRELLIS_16_STATE;
	invalid.transmit.nonlinear_parameter =
		(enum v34_nonlinear_parameter)99;
	CHECK(!v34_data_mode_is_valid(&invalid),
	      "invalid nonlinear parameter was accepted");
	invalid.transmit.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO;
	invalid.transmit.precoder_coefficients[0].real = INT16_MIN;
	CHECK(!v34_data_mode_is_valid(&invalid),
	      "invalid precoder coefficient was accepted");
	invalid.transmit.precoder_coefficients[0].real = 0;
	invalid.receive.primary_data_rate_2400_bps_units = 5;
	CHECK(!v34_data_mode_is_valid(&invalid),
	      "asymmetric rates were accepted without negotiation");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "complementary startup modes",
		  test_complementary_startup_modes },
		{ "symmetric selection", test_symmetric_selection },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
