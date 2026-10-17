// SPDX-License-Identifier: BSD-3-Clause

#include <v34_power_calibration.h>
#include <v34_rate_profile.h>
#include <v34_training_line.h>

#include "test.h"
#include "v34_power_fixture.h"
#include "v34_encoder_test_helpers.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct calibration_fixture {
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
};

static bool schedules_are_equal(
	const struct v34_frame_schedule *left,
	const struct v34_frame_schedule *right)
{
	return v34_frame_geometry_is_equal(
		       &left->geometry, &right->geometry) &&
	       left->data_frame_in_superframe ==
		       right->data_frame_in_superframe &&
	       left->next_mapping_frame == right->next_mapping_frame;
}

static bool scramblers_are_equal(
	const struct v34_scrambler *left,
	const struct v34_scrambler *right)
{
	return left->history == right->history &&
	       left->polynomial == right->polynomial;
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

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	for (unsigned int shift = 0; shift < 16u; shift += 8u) {
		hash ^= (uint8_t)(word >> shift);
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static double expected_rate_gain(uint16_t level)
{
	return (double)level * V34_TRAINING_PULSE_POINT_ZERO_Q12 /
		((double)V34_PULSE_POINT_SCALE *
		 V34_TRAINING_FIXED_POINT_ZERO);
}

static bool mean_powers_are_equal(double first, double second)
{
	double scale = fmax(1.0, fmax(fabs(first), fabs(second)));

	return fabs(first - second) <= 1e-12 * scale;
}

static bool initialize_fixture(
	struct calibration_fixture *fixture,
	enum v34_station_role role,
	enum v34_trellis_selection trellis,
	uint8_t data_frame, uint8_t mapping_frame_offset)
{
	struct calibration_fixture generated = { 0 };

	if (v34_frame_geometry_init(
		    &generated.geometry, V34_SYMBOL_RATE_3000, 8,
		    true) != V34_FRAME_SCHEDULE_OK ||
	    v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry, data_frame) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &generated.scrambler, role) != V34_FRAMING_OK ||
	    v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    trellis) != V34_4D_ENCODER_OK)
		return false;
	for (uint8_t frame = 0; frame < mapping_frame_offset; ++frame) {
		struct v34_mapping_frame ignored;

		if (v34_frame_schedule_next(
			    &generated.schedule, &ignored) !=
		    V34_FRAME_SCHEDULE_OK)
			return false;
	}
	*fixture = generated;
	return true;
}

static bool test_rate_profile(void)
{
	struct calibration_fixture ordinary;
	struct calibration_fixture expanded;
	struct v34_mapping_parameters expanded_parameters;
	struct v34_power_normalizer ordinary_normalizer;
	struct v34_power_normalizer expanded_normalizer;
	uint64_t hash = UINT64_C(14695981039346656037);
	size_t supported = 0;
	uint16_t level = 0xa5a5;

	for (unsigned int symbol_rate = 0;
	     symbol_rate < V34_SYMBOL_RATE_COUNT; ++symbol_rate) {
		for (unsigned int shaping = 0; shaping < 2u; ++shaping) {
			for (uint8_t rate = 1; rate <= V34_RATE_PROFILE_UNITS;
			     ++rate) {
				uint16_t selected = 0;
				enum v34_rate_profile_result result =
					v34_rate_profile_level(
						(enum v34_symbol_rate)symbol_rate,
						rate, shaping != 0, &selected);

				if (result == V34_RATE_PROFILE_OK)
					supported++;
				else if (result ==
					 V34_RATE_PROFILE_UNSUPPORTED_SELECTION)
					selected = 0;
				else
					CHECK(false,
					      "rate profile rejected %u/%u/%u",
					      symbol_rate, shaping, rate);
				hash = hash_word(hash, selected);
			}
		}
	}
	CHECK(supported == 130u && hash == UINT64_C(0x05ce8ea521c3e614),
	      "rate profile changed: %zu/0x%016" PRIx64, supported, hash);
	CHECK(v34_rate_profile_level(
		      (enum v34_symbol_rate)-1, 8, false, &level) ==
		      V34_RATE_PROFILE_INVALID_ARGUMENT &&
	      v34_rate_profile_level(
		      V34_SYMBOL_RATE_COUNT, 8, false, &level) ==
		      V34_RATE_PROFILE_INVALID_ARGUMENT &&
	      v34_rate_profile_level(
		      V34_SYMBOL_RATE_3000, 0, false, &level) ==
		      V34_RATE_PROFILE_INVALID_ARGUMENT &&
	      v34_rate_profile_level(
		      V34_SYMBOL_RATE_3000, 15, false, &level) ==
		      V34_RATE_PROFILE_INVALID_ARGUMENT &&
	      v34_rate_profile_level(
		      V34_SYMBOL_RATE_3429, 1, false, &level) ==
		      V34_RATE_PROFILE_UNSUPPORTED_SELECTION &&
	      v34_rate_profile_level(
		      V34_SYMBOL_RATE_3000, 8, false, NULL) ==
		      V34_RATE_PROFILE_INVALID_ARGUMENT &&
	      level == 0xa5a5,
	      "invalid rate profile changed its destination");
	CHECK(initialize_fixture(
		      &ordinary, V34_CALLING_STATION,
		      V34_TRELLIS_32_STATE, 0, 0),
	      "ordinary rate profile fixture did not initialize");
	expanded = ordinary;
	CHECK(v34_mapping_parameters_init(
		      &expanded_parameters, &expanded.geometry, true) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      v34_4d_encoder_init(
		      &expanded.encoder, &expanded_parameters, COEFFICIENTS,
		      V34_TRELLIS_32_STATE) == V34_4D_ENCODER_OK &&
	      v34_calibrate_mapping_fixture(
		      &ordinary.schedule, &ordinary.scrambler,
		      &ordinary.encoder, V34_NONLINEAR_THETA_ZERO,
		      &ordinary_normalizer) == V34_POWER_CALIBRATION_OK &&
	      v34_calibrate_mapping_fixture(
		      &expanded.schedule, &expanded.scrambler,
		      &expanded.encoder, V34_NONLINEAR_THETA_ZERO,
		      &expanded_normalizer) == V34_POWER_CALIBRATION_OK &&
	      ordinary_normalizer.normalization_gain ==
		      expected_rate_gain(0x0302) &&
	      expanded_normalizer.normalization_gain ==
		      expected_rate_gain(0x031f) &&
	      ordinary_normalizer.normalization_gain !=
		      expanded_normalizer.normalization_gain,
	      "calibration ignored the selected rate-profile half");
	return true;
}

static bool test_generated_calibration(void)
{
	struct calibration_fixture fixture;
	struct v34_power_normalizer normalizer;
	struct v34_power_normalizer repeated;
	struct v34_power_normalizer linear;
	struct v34_frame_schedule saved_schedule;
	struct v34_scrambler saved_scrambler;
	struct v34_4d_encoder saved_encoder;
	uint32_t expected_symbols;

	CHECK(initialize_fixture(
		      &fixture, V34_CALLING_STATION,
		      V34_TRELLIS_32_STATE, 3, 7),
	      "generated calibration fixture did not initialize");
	saved_schedule = fixture.schedule;
	saved_scrambler = fixture.scrambler;
	saved_encoder = fixture.encoder;
	expected_symbols =
		V34_POWER_CALIBRATION_SUPERFRAMES *
		fixture.geometry.data_frames_per_superframe *
		fixture.geometry.mapping_frames_per_data_frame *
		V34_SYMBOLS_PER_MAPPING_FRAME;
	CHECK(v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_5_OVER_16, &normalizer) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_5_OVER_16, &repeated) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, &linear) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_power_normalizer_is_valid(&normalizer) &&
	      v34_normalizers_equal(&normalizer, &repeated) &&
	      normalizer.calibration_symbol_count == expected_symbols &&
	      linear.calibration_symbol_count == 0 &&
	      linear.precoded_mean_squared_magnitude == 0.0 &&
	      linear.projected_mean_squared_magnitude == 0.0 &&
	      normalizer.projected_mean_squared_magnitude >
		      normalizer.precoded_mean_squared_magnitude &&
	      normalizer.normalization_gain < linear.normalization_gain &&
	      mean_powers_are_equal(
		      normalizer.projected_mean_squared_magnitude *
			      normalizer.normalization_gain *
			      normalizer.normalization_gain,
		      normalizer.precoded_mean_squared_magnitude *
			      linear.normalization_gain *
			      linear.normalization_gain) &&
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

static uint8_t next_calibration_bit(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return (uint8_t)(*state >> 31);
}

/* Compare the statistical derivation with actual complex-point projection,
 * using the same deterministic sample and its measured raw mean. */
static bool test_direct_projection(void)
{
	const enum v34_trellis_selection trellises[] = {
		V34_TRELLIS_16_STATE, V34_TRELLIS_32_STATE, V34_TRELLIS_64_STATE,
	};

	for (unsigned role = 0; role < 2; ++role) {
		for (unsigned t = 0; t < ARRAY_LENGTH(trellises); ++t) {
			struct calibration_fixture fixture;
			struct v34_mapping_power_calibration job;
			struct v34_power_normalizer normalizer;
			struct v34_symbol_power_measurement raw = { 0 }, projected = { 0 };

			CHECK(initialize_fixture(&fixture, role ? V34_ANSWERING_STATION :
				V34_CALLING_STATION, trellises[t], role + t, 3 * t) &&
			      v34_mapping_power_calibration_begin(&job, &fixture.schedule,
				&fixture.scrambler, &fixture.encoder,
				V34_NONLINEAR_THETA_5_OVER_16) == V34_POWER_CALIBRATION_OK,
			      "direct projection fixture failed");
			uint32_t random_state = job.random_state, frames = job.frame_count;
			CHECK(v34_mapping_power_calibration_step(&job, UINT32_MAX,
				&normalizer) == V34_POWER_CALIBRATION_OK,
			      "moment calibration failed");
			for (uint32_t frame = 0; frame < frames; ++frame) {
				struct v34_frame_schedule preview;
				struct v34_mapping_frame request;
				struct v34_transmitted_mapping_frame transmitted;
				uint8_t bits[V34_MAX_MAPPING_FRAME_BITS], auxiliary = 0;

				CHECK(v34_frame_schedule_prepare_mapping_frame(&fixture.schedule) ==
					V34_FRAME_SCHEDULE_OK, "could not prepare reference frame");
				preview = fixture.schedule;
				CHECK(v34_frame_schedule_next(&preview, &request) ==
					V34_FRAME_SCHEDULE_OK, "could not preview reference frame");
				for (unsigned bit = 0; bit < request.primary_bits; ++bit)
					bits[bit] = next_calibration_bit(&random_state);
				if (request.carries_auxiliary_bit)
					auxiliary = next_calibration_bit(&random_state);
				CHECK(v34_transmit_mapping_frame_q7(&fixture.schedule,
					&fixture.scrambler, &fixture.encoder, bits,
					request.primary_bits, request.carries_auxiliary_bit ?
					&auxiliary : NULL, &transmitted) ==
					V34_MAPPING_FRAME_TRANSMITTER_OK,
				      "reference mapping frame failed");
				for (unsigned interval = 0;
				     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME; ++interval) {
					for (unsigned symbol = 0;
					     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
						const struct v34_q7_point *point =
							&transmitted.encoded.interval[interval]
								.precoded[symbol].precoded_symbol;
						struct qam_point encoded;

						CHECK(v34_symbol_power_observe_q7(&raw, point) ==
							V34_SYMBOL_POWER_OK &&
						      v34_nonlinear_encode(V34_NONLINEAR_THETA_5_OVER_16,
							normalizer.precoded_mean_squared_magnitude,
							point, &encoded) == V34_NONLINEAR_OK &&
						      v34_symbol_power_observe_point(&projected, &encoded) ==
							V34_SYMBOL_POWER_OK, "direct projection failed");
					}
				}
			}
			CHECK(raw.symbol_count == normalizer.calibration_symbol_count &&
			      raw.mean_squared_magnitude == normalizer.precoded_mean_squared_magnitude &&
			      mean_powers_are_equal(projected.mean_squared_magnitude,
				normalizer.projected_mean_squared_magnitude),
			      "moment calibration differs from direct point projection");
		}
	}
	return true;
}

static bool test_generated_line_power(void)
{
	struct calibration_fixture fixture;
	struct v34_power_normalizer normalizer;
	struct v34_line_power_measurement unfiltered;
	struct v34_line_power_measurement repeated;
	struct v34_line_power_measurement emphasized;
	struct v34_frame_schedule saved_schedule;
	struct v34_scrambler saved_scrambler;
	struct v34_4d_encoder saved_encoder;
	struct v34_phase3_profile profile = {
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
	uint32_t expected_samples =
		(V34_POWER_CALIBRATION_SUPERFRAMES -
		 V34_LINE_POWER_WARMUP_SUPERFRAMES) *
		8000u * 280u / 1000u;

	CHECK(initialize_fixture(
		      &fixture, V34_CALLING_STATION,
		      V34_TRELLIS_32_STATE, 3, 7) &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_5_OVER_16,
		      &normalizer) == V34_POWER_CALIBRATION_OK,
	      "line-power fixture did not initialize");
	saved_schedule = fixture.schedule;
	saved_scrambler = fixture.scrambler;
	saved_encoder = fixture.encoder;
	/* Phase 4 may exceed the earlier Phase 2 projection. */
	profile.projected_data_rate_2400_bps_units = 7;
	CHECK(v34_calibrate_line_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      &normalizer, &profile, 8000, &unfiltered) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_calibrate_line_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      &normalizer, &profile, 8000, &repeated) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_line_power_measurement_is_valid(&unfiltered) &&
	      unfiltered.mean_squared_amplitude ==
		      repeated.mean_squared_amplitude &&
	      unfiltered.peak_amplitude == repeated.peak_amplitude &&
	      mean_powers_are_equal(unfiltered.mean_squared_amplitude,
				   0x1.3c1005a350754p-4) &&
	      unfiltered.peak_amplitude == 0x1.b2c8p-1 &&
	      unfiltered.sample_count == expected_samples &&
	      repeated.sample_count == expected_samples &&
	      schedules_are_equal(&fixture.schedule, &saved_schedule) &&
	      scramblers_are_equal(&fixture.scrambler, &saved_scrambler) &&
	      v34_test_encoders_are_equal(&fixture.encoder, &saved_encoder),
	      "generated line-power calibration changed");
	profile.preemphasis_filter_index = 5;
	CHECK(v34_calibrate_line_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      &normalizer, &profile, 8000, &emphasized) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_line_power_measurement_is_valid(&emphasized) &&
	      emphasized.sample_count == expected_samples &&
	      emphasized.mean_squared_amplitude !=
		      unfiltered.mean_squared_amplitude &&
	      mean_powers_are_equal(emphasized.mean_squared_amplitude,
				   0x1.6a8bc546d1addp-4) &&
	      emphasized.peak_amplitude == 0x1.e07p-1,
	      "line-power calibration ignored pre-emphasis");
	return true;
}

static bool test_line_validation(void)
{
	static const uint32_t INVALID_SAMPLE_RATES[] = {
		0, 7000, 7999, 8001, 16000, UINT32_MAX,
	};
	struct calibration_fixture fixture;
	struct v34_power_normalizer normalizer;
	struct v34_power_normalizer invalid_normalizer;
	struct v34_line_power_measurement measurement;
	struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
	struct v34_phase3_profile profile;
	struct v34_phase3_profile other_rate;
	struct v34_phase3_profile other_role;
	struct v34_phase3_profile invalid_profile;

	CHECK(initialize_fixture(&fixture, V34_CALLING_STATION,
				 V34_TRELLIS_16_STATE, 0, 0) &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_OK &&
	      v34_phase3_profile_init(&profile, V34_CALLING_STATION,
		      V34_SYMBOL_RATE_3000, &probe, 0, 0) ==
		      V34_PHASE3_PROFILE_OK &&
	      v34_phase3_profile_init(&other_rate, V34_CALLING_STATION,
		      V34_SYMBOL_RATE_2800, &probe, 0, 0) ==
		      V34_PHASE3_PROFILE_OK,
	      "line validation fixture did not initialize");
	other_role = profile;
	other_role.local_role = V34_ANSWERING_STATION;
	invalid_profile = profile;
	invalid_profile.preemphasis_filter_index =
		V34_PREEMPHASIS_MAX_FILTER_INDEX + 1u;
	invalid_normalizer = normalizer;
	invalid_normalizer.maximum_normalized_point_power = NAN;
	memset(&measurement, 0xa5, sizeof(measurement));
	CHECK(v34_calibrate_line_fixture(NULL, &fixture.scrambler,
		      &fixture.encoder, &normalizer, &profile, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, NULL,
		      &fixture.encoder, &normalizer, &profile, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      NULL, &normalizer, &profile, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, NULL, &profile, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &normalizer, NULL, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &normalizer, &profile, 8000,
		      NULL) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &normalizer, &other_rate, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &normalizer, &other_role, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &normalizer, &invalid_profile, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &invalid_normalizer, &profile, 8000,
		      &measurement) == V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      bytes_are_filled(&measurement, sizeof(measurement), 0xa5),
	      "invalid line calibration changed its destination");
	for (size_t index = 0; index < ARRAY_LENGTH(INVALID_SAMPLE_RATES);
	     ++index) {
		CHECK(v34_calibrate_line_fixture(&fixture.schedule,
			      &fixture.scrambler, &fixture.encoder, &normalizer,
			      &profile, INVALID_SAMPLE_RATES[index], &measurement) ==
			      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
		      bytes_are_filled(&measurement, sizeof(measurement), 0xa5),
		      "unsupported line sample rate %u changed its destination",
		      INVALID_SAMPLE_RATES[index]);
	}
	return true;
}

static bool test_configuration_sweep(void)
{
	static const enum v34_station_role ROLES[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const enum v34_trellis_selection TRELLISES[] = {
		V34_TRELLIS_16_STATE,
		V34_TRELLIS_32_STATE,
		V34_TRELLIS_64_STATE,
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
				struct v34_power_normalizer normalizer;
				enum v34_nonlinear_parameter parameter =
					nonlinear != 0 ?
						V34_NONLINEAR_THETA_5_OVER_16 :
						V34_NONLINEAR_THETA_ZERO;
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
				      v34_calibrate_mapping_fixture(
					      &fixture.schedule, &fixture.scrambler,
					      &fixture.encoder, parameter,
					      &normalizer) ==
					      V34_POWER_CALIBRATION_OK &&
				      v34_power_normalizer_is_valid(
					      &normalizer),
				      "calibration configuration %zu/%zu/%u failed",
				      role_index, trellis_index, nonlinear);
				if (nonlinear == 0) {
					CHECK(normalizer.calibration_symbol_count == 0 &&
					      normalizer.precoded_mean_squared_magnitude == 0.0 &&
					      normalizer.projected_mean_squared_magnitude == 0.0,
					      "linear calibration %zu/%zu changed",
					      role_index, trellis_index);
				} else {
					raw_mean[role_index][trellis_index] =
						normalizer.precoded_mean_squared_magnitude;
					CHECK(normalizer.calibration_symbol_count != 0 &&
					      raw_mean[role_index][trellis_index] > 0.0 &&
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

static bool test_resumable_calibration(void)
{
	static const uint32_t BUDGETS[] = { 1, 17, UINT32_MAX };
	struct calibration_fixture fixture;
	struct v34_phase3_profile profile = {
		.symbol_rate_numerator = 3000, .symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000, .carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = 8, .high_carrier = true,
	};

	CHECK(initialize_fixture(&fixture, V34_CALLING_STATION,
		V34_TRELLIS_32_STATE, 3, 15), "resumable fixture failed");
	for (unsigned int nonlinear = 0; nonlinear < 2; ++nonlinear) {
		enum v34_nonlinear_parameter parameter = nonlinear ?
			V34_NONLINEAR_THETA_5_OVER_16 : V34_NONLINEAR_THETA_ZERO;
		struct v34_power_normalizer expected;
		struct v34_line_power_measurement expected_line;

		CHECK(v34_calibrate_mapping_fixture(&fixture.schedule, &fixture.scrambler,
			&fixture.encoder, parameter, &expected) == V34_POWER_CALIBRATION_OK &&
		      v34_calibrate_line_fixture(&fixture.schedule, &fixture.scrambler,
			&fixture.encoder, &expected, &profile, 8000, &expected_line) ==
			V34_POWER_CALIBRATION_OK, "reference calibration failed");
		for (size_t i = 0; i < ARRAY_LENGTH(BUDGETS); ++i) {
			struct v34_mapping_power_calibration mapping;
			struct v34_line_power_calibration line;
			struct v34_power_normalizer actual;
			struct v34_line_power_measurement actual_line;
			enum v34_power_calibration_result result;
			uint32_t previous = 0, previous_frames = 0, steps = 0;

			CHECK(v34_mapping_power_calibration_begin(&mapping, &fixture.schedule,
				&fixture.scrambler, &fixture.encoder, parameter) ==
				V34_POWER_CALIBRATION_OK, "mapping job failed to begin");
			memset(&actual, 0xa5, sizeof(actual));
			CHECK(v34_mapping_power_calibration_step(NULL, 1, &actual) ==
				      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
			      v34_mapping_power_calibration_step(&mapping, 0, &actual) ==
				      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
			      v34_mapping_power_calibration_step(&mapping, 1, NULL) ==
				      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
			      bytes_are_filled(&actual, sizeof(actual), 0xa5),
			      "rejected mapping step exposed output");
			if (!nonlinear) {
				for (unsigned corrupt = 0; corrupt < 3; ++corrupt) {
					struct v34_mapping_power_calibration invalid = mapping;
					if (corrupt == 0)
						invalid.schedule.next_mapping_frame = UINT8_MAX;
					if (corrupt == 1)
						invalid.scrambler.polynomial = (enum v34_scrambler_polynomial)2;
					if (corrupt == 2)
						invalid.encoder.precoder.grid_step = 0;
					CHECK(v34_mapping_power_calibration_step(&invalid, 1, &actual) ==
						V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
					      bytes_are_filled(&actual, sizeof(actual), 0xa5),
					      "linear shortcut bypassed working-state validation %u", corrupt);
				}
			}
			do {
				result = v34_mapping_power_calibration_step(&mapping, BUDGETS[i], &actual);
				uint32_t symbols = mapping.precoded.symbol_count;
				uint32_t charged_frames = mapping.frame +
					(mapping.measuring_projected ? mapping.frame_count : 0);
				CHECK(charged_frames - previous_frames <= BUDGETS[i],
				      "mapping cadence exceeded budget");
				previous_frames = charged_frames;
				CHECK(symbols - previous <= (uint64_t)BUDGETS[i] *
					V34_SYMBOLS_PER_MAPPING_FRAME && ++steps <= 1000,
					"mapping step exceeded budget or failed to progress");
				previous = symbols;
				if (result == V34_POWER_CALIBRATION_IN_PROGRESS)
					CHECK(bytes_are_filled(&actual, sizeof(actual), 0xa5),
						"pending mapping exposed partial output");
			} while (result == V34_POWER_CALIBRATION_IN_PROGRESS);
			CHECK(result == V34_POWER_CALIBRATION_OK &&
			      steps == ((uint64_t)mapping.frame_count * (nonlinear ? 2u : 1u) +
				BUDGETS[i] - 1u) / BUDGETS[i] &&
			      v34_normalizers_equal(&actual, &expected),
			      "resuming changed mapping calibration");
			CHECK(v34_line_power_calibration_begin(&line, &fixture.schedule,
				&fixture.scrambler, &fixture.encoder, &actual, &profile, 8000) ==
				V34_POWER_CALIBRATION_OK, "line job failed to begin");
			memset(&actual_line, 0xa5, sizeof(actual_line));
			CHECK(v34_line_power_calibration_step(NULL, 1, &actual_line) ==
				      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
			      v34_line_power_calibration_step(&line, 0, &actual_line) ==
				      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
			      v34_line_power_calibration_step(&line, 1, NULL) ==
				      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
			      bytes_are_filled(&actual_line, sizeof(actual_line), 0xa5),
			      "rejected line step exposed output");
			steps = 0;
			do {
				uint32_t frame = line.frame;
				result = v34_line_power_calibration_step(&line, BUDGETS[i], &actual_line);
				CHECK(line.frame - frame <= BUDGETS[i] && ++steps <= 1000,
					"line step exceeded budget or failed to progress");
				if (result == V34_POWER_CALIBRATION_IN_PROGRESS)
					CHECK(bytes_are_filled(&actual_line, sizeof(actual_line), 0xa5),
						"pending line job exposed partial output");
			} while (result == V34_POWER_CALIBRATION_IN_PROGRESS);
			CHECK(result == V34_POWER_CALIBRATION_OK &&
			      actual_line.mean_squared_amplitude == expected_line.mean_squared_amplitude &&
			      actual_line.peak_amplitude == expected_line.peak_amplitude &&
			      actual_line.sample_count == expected_line.sample_count,
			      "resuming changed line calibration");
		}
	}
	return true;
}

static bool test_linear_range_fallback(void)
{
	struct calibration_fixture fixture;
	struct v34_mapping_power_calibration calibration;
	struct v34_power_normalizer normalizer;

	CHECK(initialize_fixture(&fixture, V34_CALLING_STATION,
		V34_TRELLIS_16_STATE, 0, 0), "linear fallback fixture failed");
	memset(fixture.encoder.precoder.coefficients, 0,
		sizeof(fixture.encoder.precoder.coefficients));
	/* A large initial history prevents the proof, but zero coefficients make
	 * the generated stream safe. Keep accepting it through measured fallback. */
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap)
		fixture.encoder.precoder.previous_symbols[tap] =
			(struct v34_q7_point){ 254 * V34_Q7_SCALE, 254 * V34_Q7_SCALE };
	CHECK(v34_mapping_power_calibration_begin(&calibration, &fixture.schedule,
		&fixture.scrambler, &fixture.encoder, V34_NONLINEAR_THETA_ZERO) ==
		V34_POWER_CALIBRATION_OK && !calibration.linear_bounded &&
	      v34_mapping_power_calibration_step(&calibration, UINT32_MAX, &normalizer) ==
		V34_POWER_CALIBRATION_OK && v34_power_normalizer_is_valid(&normalizer) &&
	      normalizer.calibration_symbol_count ==
		calibration.frame_count * V34_SYMBOLS_PER_MAPPING_FRAME &&
	      normalizer.precoded_mean_squared_magnitude > 0.0 &&
	      normalizer.precoded_mean_squared_magnitude ==
		normalizer.projected_mean_squared_magnitude,
	      "unproven linear range bypassed generated validation");
	return true;
}

static bool test_transactional_validation(void)
{
	struct calibration_fixture fixture;
	struct calibration_fixture mismatched;
	struct calibration_fixture out_of_range;
	struct v34_frame_schedule invalid_schedule;
	struct v34_scrambler invalid_scrambler;
	struct v34_4d_encoder invalid_encoder;
	struct v34_power_normalizer normalizer;
	struct v34_q14_coefficient range_coefficients
		[V34_PRECODER_TAP_COUNT] = { 0 };

	CHECK(initialize_fixture(
		      &fixture, V34_CALLING_STATION,
		      V34_TRELLIS_16_STATE, 0, 0) &&
	      initialize_fixture(
		      &mismatched, V34_CALLING_STATION,
		      V34_TRELLIS_16_STATE, 0, 0) &&
	      initialize_fixture(
		      &out_of_range, V34_CALLING_STATION,
		      V34_TRELLIS_16_STATE, 0, 0),
	      "calibration validation fixtures did not initialize");
	CHECK(v34_frame_geometry_init(
		      &mismatched.geometry, V34_SYMBOL_RATE_3000, 7,
		      true) == V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &mismatched.parameters, &mismatched.geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      v34_4d_encoder_init(
		      &mismatched.encoder, &mismatched.parameters, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_OK,
	      "mismatched calibration encoder did not initialize");
	range_coefficients[2].real = V34_Q14_SCALE;
	CHECK(v34_4d_encoder_init(
		      &out_of_range.encoder, &out_of_range.parameters,
		      range_coefficients, V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_OK,
	      "range calibration encoder did not initialize");
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		out_of_range.encoder.precoder.previous_symbols[tap] =
			(struct v34_q7_point){
				254 * V34_Q7_SCALE,
				254 * V34_Q7_SCALE,
			};
	}
	CHECK(v34_4d_encoder_is_valid(&out_of_range.encoder),
	      "range calibration encoder became invalid");
	invalid_schedule = fixture.schedule;
	invalid_schedule.next_mapping_frame = (uint8_t)(
		invalid_schedule.geometry.mapping_frames_per_data_frame + 1u);
	invalid_scrambler = fixture.scrambler;
	invalid_scrambler.polynomial =
		(enum v34_scrambler_polynomial)2;
	invalid_encoder = fixture.encoder;
	invalid_encoder.previous_differential_output = 4;
	memset(&normalizer, 0xa5, sizeof(normalizer));
	CHECK(v34_calibrate_mapping_fixture(
		      NULL, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, NULL, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, NULL,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, NULL) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &invalid_schedule, &fixture.scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &invalid_scrambler, &fixture.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &invalid_encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler,
		      &mismatched.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      v34_calibrate_mapping_fixture(
		      &fixture.schedule, &fixture.scrambler, &fixture.encoder,
		      (enum v34_nonlinear_parameter)2,
		      &normalizer) ==
		      V34_POWER_CALIBRATION_INVALID_ARGUMENT &&
	      bytes_are_filled(&normalizer, sizeof(normalizer), 0xa5),
	      "invalid calibration changed its destination");
	CHECK(v34_calibrate_mapping_fixture(
		      &out_of_range.schedule, &out_of_range.scrambler,
		      &out_of_range.encoder,
		      V34_NONLINEAR_THETA_ZERO, &normalizer) ==
		      V34_POWER_CALIBRATION_OUT_OF_RANGE &&
	      bytes_are_filled(&normalizer, sizeof(normalizer), 0xa5),
	      "range calibration changed its destination");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "rate profile", test_rate_profile },
		{ "generated calibration", test_generated_calibration },
		{ "direct point projection", test_direct_projection },
		{ "generated line power", test_generated_line_power },
		{ "line validation", test_line_validation },
		{ "configuration sweep", test_configuration_sweep },
		{ "resumable calibration", test_resumable_calibration },
		{ "linear range fallback", test_linear_range_fallback },
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
