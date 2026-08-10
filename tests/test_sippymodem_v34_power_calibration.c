// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_power_calibration.h>

#include "test.h"
#include "v34_encoder_test_helpers.h"

#include <stddef.h>

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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated calibration", test_generated_calibration },
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
