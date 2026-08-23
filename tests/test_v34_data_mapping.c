// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_mapping.h>
#include <v34_data_line_transmitter.h>
#include <v34_training_line.h>

#include "test.h"
#include "v34_encoder_test_helpers.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct v34_phase3_profile profile_fixture(
	enum v34_station_role role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.transmit_power_reduction_db = 2,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 12,
		.high_carrier = true,
	};
}

static struct v34_data_direction_parameters parameters_fixture(void)
{
	return (struct v34_data_direction_parameters){
		.precoder_coefficients = {
			{ 4096, 2048 },
			{ -2048, 1024 },
			{ 1024, -2048 },
		},
		.trellis_selection = V34_TRELLIS_32_STATE,
		.nonlinear_parameter = V34_NONLINEAR_THETA_5_OVER_16,
		.primary_data_rate_2400_bps_units = 8,
		.expanded_shaping = true,
	};
}

static bool normalizers_are_equal(
	const struct v34_power_normalizer *left,
	const struct v34_power_normalizer *right)
{
	return v34_mapping_parameters_are_equal(
		       &left->parameters, &right->parameters) &&
	       left->precoded_mean_squared_magnitude ==
		       right->precoded_mean_squared_magnitude &&
	       left->projected_mean_squared_magnitude ==
		       right->projected_mean_squared_magnitude &&
	       left->normalization_gain == right->normalization_gain &&
	       left->maximum_precoded_point_power ==
		       right->maximum_precoded_point_power &&
	       left->maximum_projected_point_power ==
		       right->maximum_projected_point_power &&
	       left->maximum_normalized_point_power ==
		       right->maximum_normalized_point_power &&
	       left->calibration_symbol_count ==
		       right->calibration_symbol_count &&
	       left->nonlinear_parameter == right->nonlinear_parameter;
}

static bool test_prepared_b1_mapping(void)
{
	struct v34_phase3_profile caller =
		profile_fixture(V34_CALLING_STATION);
	struct v34_phase3_profile answerer =
		profile_fixture(V34_ANSWERING_STATION);
	struct v34_data_direction_parameters parameters = parameters_fixture();
	struct v34_data_mapping first;
	struct v34_data_mapping repeated;
	struct v34_data_mapping reverse;
	struct v34_training_line training;
	struct v34_data_line_transmitter continued;
	double point_gain;
	double repeated_gain;

	CHECK(v34_data_mapping_prepare(
		      &first, &caller, &parameters, true) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_prepare(
		      &repeated, &caller, &parameters, true) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_prepare(
		      &reverse, &answerer, &parameters, true) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_is_valid(&first) &&
	      v34_data_mapping_is_valid(&repeated) &&
	      v34_data_mapping_is_valid(&reverse),
	      "valid directional mappings were not prepared");
	CHECK(first.schedule.geometry.symbol_rate == V34_SYMBOL_RATE_3000 &&
	      first.schedule.geometry.primary_data_rate_2400_bps_units == 8 &&
	      first.schedule.geometry.auxiliary_channel_enabled &&
	      first.schedule.data_frame_in_superframe + 1u ==
		      first.schedule.geometry.data_frames_per_superframe &&
	      first.schedule.next_mapping_frame == 0 &&
	      first.scrambler.history == 0 &&
	      first.scrambler.polynomial == V34_POLYNOMIAL_GPC &&
	      reverse.scrambler.polynomial == V34_POLYNOMIAL_GPA,
	      "prepared mapping did not begin at the directional B1 boundary");
	CHECK(first.encoder.convolutional_encoder.selection ==
		      parameters.trellis_selection &&
	      first.encoder.convolutional_encoder.state == 0 &&
	      first.encoder.previous_differential_output == 0 &&
	      memcmp(first.encoder.precoder.coefficients,
		     parameters.precoder_coefficients,
		     sizeof(parameters.precoder_coefficients)) == 0 &&
	      first.normalizer.nonlinear_parameter ==
		      parameters.nonlinear_parameter &&
	      v34_test_encoders_are_equal(&first.encoder, &repeated.encoder) &&
	      normalizers_are_equal(&first.normalizer, &repeated.normalizer),
	      "directional mapping state or deterministic calibration changed");
	CHECK(v34_data_mapping_line_point_gain(
		      &first, 8000, &point_gain) == V34_DATA_MAPPING_OK &&
	      v34_data_mapping_line_point_gain(
		      &first, 8000, &repeated_gain) == V34_DATA_MAPPING_OK &&
	      v34_training_line_init(&training, &caller, 8000, 5000) ==
		      V34_TRAINING_LINE_OK &&
	      v34_data_line_transmitter_continue(
		      &continued, &training, &first.schedule,
		      &first.scrambler, &first.encoder, &first.normalizer) ==
		      V34_DATA_LINE_TRANSMITTER_OK &&
	      point_gain == 1.0 && repeated_gain == 1.0 &&
	      continued.carrier.point_gain == 1.0,
	      "B1 mapping changed the trained constellation scale");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_profile profile =
		profile_fixture(V34_CALLING_STATION);
	struct v34_data_direction_parameters parameters = parameters_fixture();
	struct v34_data_mapping destination;
	struct v34_data_mapping prepared;
	double point_gain = 123.0;
	unsigned char saved[sizeof(destination)];

	memset(&destination, 0xa5, sizeof(destination));
	memcpy(saved, &destination, sizeof(saved));
	CHECK(v34_data_mapping_prepare(
		      NULL, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      v34_data_mapping_prepare(
		      &destination, NULL, &parameters, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      v34_data_mapping_prepare(
		      &destination, &profile, NULL, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      memcmp(&destination, saved, sizeof(destination)) == 0,
	      "invalid arguments changed the mapping destination");
	parameters.precoder_coefficients[0].real = INT16_MIN;
	CHECK(v34_data_mapping_prepare(
		      &destination, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      memcmp(&destination, saved, sizeof(destination)) == 0,
	      "invalid direction parameters changed the destination");
	parameters = parameters_fixture();
	parameters.primary_data_rate_2400_bps_units = 14;
	profile.projected_data_rate_2400_bps_units = 14;
	CHECK(v34_data_mapping_prepare(
		      &destination, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OUT_OF_RANGE &&
	      memcmp(&destination, saved, sizeof(destination)) == 0,
	      "unsupported symbol-rate geometry changed the destination");
	parameters = parameters_fixture();
	profile = profile_fixture(V34_CALLING_STATION);
	profile.projected_data_rate_2400_bps_units = 4;
	CHECK(v34_data_mapping_prepare(
		      &prepared, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_is_valid(&prepared) &&
	      prepared.schedule.geometry
			      .primary_data_rate_2400_bps_units == 8,
	      "Phase 2 projection incorrectly limited Phase 4 mapping");

	parameters = parameters_fixture();
	profile = profile_fixture(V34_CALLING_STATION);
	CHECK(v34_data_mapping_prepare(
		      &prepared, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OK,
	      "validation fixture did not initialize");
	prepared.scrambler.history = 1;
	CHECK(!v34_data_mapping_is_valid(NULL) &&
	      !v34_data_mapping_is_valid(&prepared),
	      "advanced scrambler was accepted as a fresh B1 mapping");
	CHECK(v34_data_mapping_prepare(
		      &prepared, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OK,
	      "validation fixture did not reinitialize");
	CHECK(v34_data_mapping_line_point_gain(
		      &prepared, 0, &point_gain) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      v34_data_mapping_line_point_gain(
		      NULL, 8000, &point_gain) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      v34_data_mapping_line_point_gain(
		      &prepared, 8000, NULL) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      point_gain == 123.0,
	      "invalid line point-gain input changed its output");
	prepared.encoder.previous_differential_output = 1;
	CHECK(!v34_data_mapping_is_valid(&prepared) &&
	      v34_data_mapping_line_point_gain(
		      &prepared, 8000, &point_gain) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      point_gain == 123.0,
	      "advanced encoder was accepted by line point calibration");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "prepared B1 mapping", test_prepared_b1_mapping },
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
