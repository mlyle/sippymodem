// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_power_calibration.h>

#include "test.h"
#include "v34_encoder_test_helpers.h"

#include <stddef.h>
#include <string.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct calibration_fixture {
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
};

static bool schedules_are_equal(
	const struct sippymodem_v34_frame_schedule *left,
	const struct sippymodem_v34_frame_schedule *right)
{
	return left->geometry.symbol_rate == right->geometry.symbol_rate &&
	       left->geometry.data_bits_per_frame ==
		       right->geometry.data_bits_per_frame &&
	       left->geometry.primary_data_rate_2400_bps_units ==
		       right->geometry.primary_data_rate_2400_bps_units &&
	       left->geometry.data_frames_per_superframe ==
		       right->geometry.data_frames_per_superframe &&
	       left->geometry.mapping_frames_per_data_frame ==
		       right->geometry.mapping_frames_per_data_frame &&
	       left->geometry.high_mapping_frame_bits ==
		       right->geometry.high_mapping_frame_bits &&
	       left->geometry.high_mapping_frame_count ==
		       right->geometry.high_mapping_frame_count &&
	       left->geometry.auxiliary_bits_per_frame ==
		       right->geometry.auxiliary_bits_per_frame &&
	       left->geometry.auxiliary_channel_enabled ==
		       right->geometry.auxiliary_channel_enabled &&
	       left->data_frame_in_superframe ==
		       right->data_frame_in_superframe &&
	       left->next_mapping_frame == right->next_mapping_frame;
}

static bool scramblers_are_equal(
	const struct sippymodem_v34_scrambler *left,
	const struct sippymodem_v34_scrambler *right)
{
	return left->history == right->history &&
	       left->polynomial == right->polynomial;
}

static bool normalizers_are_equal(
	const struct sippymodem_v34_power_normalizer *left,
	const struct sippymodem_v34_power_normalizer *right)
{
	return v34_test_mapping_parameters_are_equal(
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

static bool bytes_are_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != value)
			return false;
	}
	return true;
}

static bool initialize_fixture(
	struct calibration_fixture *fixture,
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_trellis_selection trellis,
	uint8_t data_frame, uint8_t mapping_frame_offset)
{
	struct calibration_fixture generated = { 0 };

	if (sippymodem_v34_frame_geometry_init(
		    &generated.geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8,
		    true) != SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry, data_frame) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_transmit_scrambler_init(
		    &generated.scrambler, role) != SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    trellis) != SIPPYMODEM_V34_4D_ENCODER_OK)
		return false;
	for (uint8_t frame = 0; frame < mapping_frame_offset; ++frame) {
		struct sippymodem_v34_mapping_frame ignored;

		if (sippymodem_v34_frame_schedule_next(
			    &generated.schedule, &ignored) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
			return false;
	}
	*fixture = generated;
	return true;
}

static bool test_generated_calibration(void)
{
	struct calibration_fixture fixture;
	struct sippymodem_v34_power_normalizer normalizer;
	struct sippymodem_v34_power_normalizer repeated;
	struct sippymodem_v34_power_normalizer linear;
	struct sippymodem_v34_frame_schedule saved_schedule;
	struct sippymodem_v34_scrambler saved_scrambler;
	struct sippymodem_v34_4d_encoder saved_encoder;
	uint32_t expected_symbols;

	CHECK(initialize_fixture(
		      &fixture, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_TRELLIS_32_STATE, 3, 7),
	      "generated calibration fixture did not initialize");
	saved_schedule = fixture.schedule;
	saved_scrambler = fixture.scrambler;
	saved_encoder = fixture.encoder;
	expected_symbols =
		SIPPYMODEM_V34_POWER_CALIBRATION_SUPERFRAMES *
		fixture.geometry.data_frames_per_superframe *
		fixture.geometry.mapping_frames_per_data_frame *
		SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME;
	CHECK(sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_OK &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, &repeated) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_OK &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &linear) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_OK &&
	      sippymodem_v34_power_normalizer_is_valid(&normalizer) &&
	      normalizers_are_equal(&normalizer, &repeated) &&
	      normalizer.calibration_symbol_count == expected_symbols &&
	      normalizer.precoded_mean_squared_magnitude ==
		      linear.precoded_mean_squared_magnitude &&
	      linear.precoded_mean_squared_magnitude ==
		      linear.projected_mean_squared_magnitude &&
	      normalizer.projected_mean_squared_magnitude >
		      normalizer.precoded_mean_squared_magnitude &&
	      normalizer.precoded_mean_squared_magnitude <=
		      normalizer.maximum_precoded_point_power &&
	      normalizer.projected_mean_squared_magnitude <=
		      normalizer.maximum_projected_point_power &&
	      schedules_are_equal(&fixture.schedule, &saved_schedule) &&
	      scramblers_are_equal(&fixture.scrambler, &saved_scrambler) &&
	      v34_test_encoders_are_equal(&fixture.encoder, &saved_encoder),
	      "generated two-pass calibration changed");
	return true;
}

static bool test_configuration_sweep(void)
{
	static const enum sippymodem_v34_station_role ROLES[] = {
		SIPPYMODEM_V34_CALLING_STATION,
		SIPPYMODEM_V34_ANSWERING_STATION,
	};
	static const enum sippymodem_v34_trellis_selection TRELLISES[] = {
		SIPPYMODEM_V34_TRELLIS_16_STATE,
		SIPPYMODEM_V34_TRELLIS_32_STATE,
		SIPPYMODEM_V34_TRELLIS_64_STATE,
	};
	double raw_mean[ARRAY_LENGTH(ROLES)][ARRAY_LENGTH(TRELLISES)] = { 0 };
	bool completed_start_seen = false;
	size_t configuration_count = 0;

	for (size_t role_index = 0; role_index < ARRAY_LENGTH(ROLES);
	     ++role_index) {
		for (size_t trellis_index = 0;
		     trellis_index < ARRAY_LENGTH(TRELLISES); ++trellis_index) {
			for (unsigned int nonlinear = 0; nonlinear < 2;
			     ++nonlinear) {
				struct calibration_fixture fixture;
				struct sippymodem_v34_power_normalizer normalizer;
				enum sippymodem_v34_nonlinear_parameter parameter =
					nonlinear != 0 ?
						SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16 :
						SIPPYMODEM_V34_NONLINEAR_THETA_ZERO;
				uint8_t data_frame = (uint8_t)(
					(role_index * ARRAY_LENGTH(TRELLISES) +
					 trellis_index) %
					7u);
				uint8_t mapping_frame_offset = (uint8_t)(
					(role_index * 3u + trellis_index * 5u) % 15u);

				if (role_index == 1u && trellis_index == 2u) {
					mapping_frame_offset = 15;
					completed_start_seen = true;
				}
				CHECK(initialize_fixture(
					      &fixture, ROLES[role_index],
					      TRELLISES[trellis_index], data_frame,
					      mapping_frame_offset) &&
				      sippymodem_v34_calibrate_mapping_power(
					      &fixture.schedule, &fixture.scrambler,
					      &fixture.encoder, parameter,
					      &normalizer) ==
					      SIPPYMODEM_V34_POWER_CALIBRATION_OK &&
				      sippymodem_v34_power_normalizer_is_valid(
					      &normalizer),
				      "calibration configuration %zu/%zu/%u failed",
				      role_index, trellis_index, nonlinear);
				if (nonlinear == 0) {
					raw_mean[role_index][trellis_index] =
						normalizer
							.precoded_mean_squared_magnitude;
					CHECK(normalizer
						      .projected_mean_squared_magnitude ==
						      normalizer
							      .precoded_mean_squared_magnitude,
					      "linear calibration %zu/%zu changed",
					      role_index, trellis_index);
				} else {
					CHECK(normalizer
						      .precoded_mean_squared_magnitude ==
						      raw_mean[role_index][trellis_index] &&
					      normalizer
						      .projected_mean_squared_magnitude >
						      normalizer
							      .precoded_mean_squared_magnitude,
					      "nonlinear calibration %zu/%zu changed raw data",
					      role_index, trellis_index);
				}
				configuration_count++;
			}
		}
	}
	CHECK(configuration_count == 12u && completed_start_seen &&
	      raw_mean[0][0] != raw_mean[1][2],
	      "calibration sweep missed a generated stream boundary");
	return true;
}

static bool test_transactional_validation(void)
{
	struct calibration_fixture fixture;
	struct calibration_fixture mismatched;
	struct calibration_fixture out_of_range;
	struct sippymodem_v34_frame_schedule invalid_schedule;
	struct sippymodem_v34_scrambler invalid_scrambler;
	struct sippymodem_v34_4d_encoder invalid_encoder;
	struct sippymodem_v34_power_normalizer normalizer;
	struct sippymodem_v34_q14_coefficient range_coefficients
		[SIPPYMODEM_V34_PRECODER_TAP_COUNT] = { 0 };

	CHECK(initialize_fixture(
		      &fixture, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_TRELLIS_16_STATE, 0, 0) &&
	      initialize_fixture(
		      &mismatched, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_TRELLIS_16_STATE, 0, 0) &&
	      initialize_fixture(
		      &out_of_range, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_TRELLIS_16_STATE, 0, 0),
	      "calibration validation fixtures did not initialize");
	CHECK(sippymodem_v34_frame_geometry_init(
		      &mismatched.geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 7,
		      true) == SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &mismatched.parameters, &mismatched.geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	      sippymodem_v34_4d_encoder_init(
		      &mismatched.encoder, &mismatched.parameters, COEFFICIENTS,
		      SIPPYMODEM_V34_TRELLIS_16_STATE) ==
		      SIPPYMODEM_V34_4D_ENCODER_OK,
	      "mismatched calibration encoder did not initialize");
	range_coefficients[2].real = SIPPYMODEM_V34_Q14_SCALE;
	CHECK(sippymodem_v34_4d_encoder_init(
		      &out_of_range.encoder, &out_of_range.parameters,
		      range_coefficients, SIPPYMODEM_V34_TRELLIS_16_STATE) ==
		      SIPPYMODEM_V34_4D_ENCODER_OK,
	      "range calibration encoder did not initialize");
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT; ++tap) {
		out_of_range.encoder.precoder.previous_symbols[tap] =
			(struct sippymodem_v34_q7_point){
				254 * SIPPYMODEM_V34_Q7_SCALE,
				254 * SIPPYMODEM_V34_Q7_SCALE,
			};
	}
	CHECK(sippymodem_v34_4d_encoder_is_valid(&out_of_range.encoder),
	      "range calibration encoder became invalid");
	invalid_schedule = fixture.schedule;
	invalid_schedule.next_mapping_frame = (uint8_t)(
		invalid_schedule.geometry.mapping_frames_per_data_frame + 1u);
	invalid_scrambler = fixture.scrambler;
	invalid_scrambler.polynomial =
		(enum sippymodem_v34_scrambler_polynomial)2;
	invalid_encoder = fixture.encoder;
	invalid_encoder.previous_differential_output = 4;
	memset(&normalizer, 0xa5, sizeof(normalizer));
	CHECK(sippymodem_v34_calibrate_mapping_power(
		      NULL, &fixture.scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, NULL, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, NULL,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, NULL) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &invalid_schedule, &fixture.scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &invalid_scrambler, &fixture.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, &invalid_encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler,
		      &mismatched.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      sippymodem_v34_calibrate_mapping_power(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      (enum sippymodem_v34_nonlinear_parameter)2,
		      &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      bytes_are_filled(&normalizer, sizeof(normalizer), 0xa5),
	      "invalid calibration changed its destination");
	CHECK(sippymodem_v34_calibrate_mapping_power(
		      &out_of_range.schedule, &out_of_range.scrambler,
		      &out_of_range.encoder,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE &&
	      bytes_are_filled(&normalizer, sizeof(normalizer), 0xa5),
	      "range calibration changed its destination");
	CHECK(strcmp(sippymodem_v34_power_calibration_result_name(
		     SIPPYMODEM_V34_POWER_CALIBRATION_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_power_calibration_result_name(
		     SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_power_calibration_result_name(
		     SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(sippymodem_v34_power_calibration_result_name(
		     SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_power_calibration_result_name(
		     (enum sippymodem_v34_power_calibration_result)4),
		     "unknown") == 0,
	      "power-calibration result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated calibration", test_generated_calibration },
		{ "configuration sweep", test_configuration_sweep },
		{ "transactional validation", test_transactional_validation },
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
