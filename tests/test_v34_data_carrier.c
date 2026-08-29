// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_carrier.h>
#include <v34_data_points.h>
#include <v34_power_calibration.h>

#include "test.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct carrier_fixture {
	struct v34_phase3_profile profile;
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;
	struct v34_data_points points;
};

struct exact_profile_rates {
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t carrier_numerator;
	uint32_t carrier_denominator;
};

static struct exact_profile_rates profile_rates(
	enum v34_symbol_rate rate, bool high_carrier)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		return (struct exact_profile_rates){
			2400, 1, high_carrier ? 1800u : 1600u, 1,
		};
	case V34_SYMBOL_RATE_2743:
		return (struct exact_profile_rates){
			19200, 7, high_carrier ? 12800u : 11520u, 7,
		};
	case V34_SYMBOL_RATE_2800:
		return (struct exact_profile_rates){
			2800, 1, high_carrier ? 5600u : 1680u,
			high_carrier ? 3u : 1u,
		};
	case V34_SYMBOL_RATE_3000:
		return (struct exact_profile_rates){
			3000, 1, high_carrier ? 2000u : 1800u, 1,
		};
	case V34_SYMBOL_RATE_3200:
		return (struct exact_profile_rates){
			3200, 1, high_carrier ? 1920u : 12800u,
			high_carrier ? 1u : 7u,
		};
	case V34_SYMBOL_RATE_3429:
		return (struct exact_profile_rates){ 24000, 7, 96000, 49 };
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return (struct exact_profile_rates){ 0 };
}

static struct v34_phase3_profile profile_for_rate(
	enum v34_symbol_rate rate, bool high_carrier, uint8_t primary_rate)
{
	struct exact_profile_rates rates = profile_rates(rate, high_carrier);

	return (struct v34_phase3_profile){
		.symbol_rate_numerator = rates.symbol_numerator,
		.symbol_rate_denominator = rates.symbol_denominator,
		.carrier_frequency_numerator = rates.carrier_numerator,
		.carrier_frequency_denominator = rates.carrier_denominator,
		.symbol_rate = rate,
		.local_role = V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = primary_rate,
		.high_carrier = high_carrier,
	};
}

static void generate_bits(
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS], size_t count)
{
	uint32_t state = UINT32_C(0x53e3b671);

	for (size_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static bool initialize_fixture(struct carrier_fixture *fixture)
{
	struct carrier_fixture generated = { 0 };
	struct v34_mapping_frame request;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit = 1;
	const uint8_t *auxiliary;

	generated.profile = (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.transmit_power_reduction_db = 0,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
	if (fixture == NULL ||
	    !v34_phase3_profile_is_valid(&generated.profile) ||
	    v34_frame_geometry_init(
		    &generated.geometry, V34_SYMBOL_RATE_3000, 8, true) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry, 0) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &generated.scrambler, V34_CALLING_STATION) != V34_FRAMING_OK ||
	    v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    V34_TRELLIS_32_STATE) != V34_4D_ENCODER_OK ||
	    v34_calibrate_mapping_power(
		    &generated.schedule, &generated.scrambler,
		    &generated.encoder, V34_NONLINEAR_THETA_5_OVER_16,
		    &generated.normalizer) != V34_POWER_CALIBRATION_OK ||
	    v34_data_points_init(
		    &generated.points, &generated.schedule,
		    &generated.scrambler, &generated.encoder,
		    &generated.normalizer, V34_CALLING_STATION) !=
		    V34_DATA_POINTS_OK ||
	    v34_data_points_preview_mapping_frame(
		    &generated.points, &request) != V34_DATA_POINTS_OK)
		return false;
	generate_bits(primary_bits, request.primary_bits);
	auxiliary = request.carries_auxiliary_bit ? &auxiliary_bit : NULL;
	if (v34_data_points_queue_mapping_frame(
		    &generated.points, primary_bits, request.primary_bits,
		    auxiliary) != V34_DATA_POINTS_OK)
		return false;
	*fixture = generated;
	return true;
}

static bool test_carrier_point_stream(void)
{
	struct carrier_fixture fixture;
	struct v34_data_points points;
	struct v34_data_carrier carrier;
	struct v34_pulse_modulator model;
	size_t samples = 0;
	bool nonzero_seen = false;

	CHECK(initialize_fixture(&fixture) &&
	      v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 8000) == V34_DATA_CARRIER_OK &&
	      v34_data_carrier_needs_point(&carrier) &&
	      carrier.modulator.maximum_point_power ==
		      fixture.normalizer.maximum_normalized_point_power,
	      "data carrier did not initialize");
	points = fixture.points;
	model = carrier.modulator;
	while (!v34_data_points_needs_mapping_frame(&points)) {
		struct qam_point point;
		const struct qam_point *point_pointer = NULL;
		double expected;
		double actual;
		bool expected_accepted;
		bool actual_accepted;

		if (v34_data_carrier_needs_point(&carrier)) {
			CHECK(v34_data_points_next(&points, &point) ==
				      V34_DATA_POINTS_OK,
			      "point stream failed at sample %zu", samples);
			point_pointer = &point;
		}
		CHECK(samples++ < 64u &&
		      v34_pulse_modulator_sample(
			      &model, point_pointer, &expected,
			      &expected_accepted) == V34_PULSE_MODULATOR_OK &&
		      v34_data_carrier_sample(
			      &carrier, point_pointer, &actual,
			      &actual_accepted) == V34_DATA_CARRIER_OK &&
		      actual == expected &&
		      actual_accepted == expected_accepted &&
		      memcmp(&carrier.modulator, &model, sizeof(model)) == 0,
		      "data carrier diverged at sample %zu", samples - 1u);
		nonzero_seen |= actual != 0.0;
	}
	CHECK(samples > V34_SYMBOLS_PER_MAPPING_FRAME && nonzero_seen &&
	      !v34_data_carrier_needs_point(&carrier),
	      "carrier did not consume the complete point frame");
	return true;
}

static bool test_all_carrier_profiles(void)
{
	static const uint8_t PRIMARY_RATES[] = { 1, 2, 2, 2, 2, 2 };
	size_t configurations = 0;

	for (unsigned int rate_index = 0;
	     rate_index < V34_SYMBOL_RATE_COUNT; ++rate_index) {
		enum v34_symbol_rate rate = (enum v34_symbol_rate)rate_index;
		unsigned int carrier_count =
			rate == V34_SYMBOL_RATE_3429 ? 1u : 2u;

		for (unsigned int high = 0; high < carrier_count; ++high) {
			struct v34_phase3_profile profile = profile_for_rate(
				rate, high != 0, PRIMARY_RATES[rate_index]);
			struct v34_frame_geometry geometry;
			struct v34_mapping_parameters parameters;
			struct v34_symbol_power_measurement measurement = {
				.mean_squared_magnitude = 1.0,
				.symbol_count = 1,
			};
			struct v34_power_normalizer normalizer;
			struct v34_data_carrier carrier;

			CHECK(v34_phase3_profile_is_valid(&profile) &&
			      v34_frame_geometry_init(
				      &geometry, rate, PRIMARY_RATES[rate_index],
				      false) == V34_FRAME_SCHEDULE_OK &&
			      v34_mapping_parameters_init(
				      &parameters, &geometry, false) ==
				      V34_MAPPING_PARAMETERS_OK &&
			      v34_power_normalizer_init(
				      &normalizer, &parameters,
				      V34_NONLINEAR_THETA_ZERO, &measurement,
				      &measurement) == V34_SYMBOL_POWER_OK &&
			      v34_data_carrier_init(
				      &carrier, &profile, &geometry, &normalizer,
				      8000) == V34_DATA_CARRIER_OK &&
		      v34_line_channel_matches(
				      &profile, &carrier.modulator.channel),
			      "carrier profile %u/%u changed", rate_index, high);
			configurations++;
		}
	}
	CHECK(configurations == 11u,
	      "covered %zu carrier profiles", configurations);
	return true;
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

static bool test_transactional_validation(void)
{
	struct carrier_fixture fixture;
	struct v34_data_carrier carrier;
	struct v34_data_carrier carrier_before;
	struct v34_phase3_profile limited_profile;
	struct v34_frame_geometry other_geometry;
	struct qam_point point = { 0.1, -0.2 };
	struct qam_point excessive;
	double carrier_sample = 123.0;
	bool accepted = true;

	CHECK(initialize_fixture(&fixture),
	      "validation fixture did not initialize");
	limited_profile = fixture.profile;
	limited_profile.projected_data_rate_2400_bps_units = 7;
	CHECK(v34_frame_geometry_init(
		      &other_geometry, V34_SYMBOL_RATE_3000, 7, true) ==
		      V34_FRAME_SCHEDULE_OK,
	      "other geometry did not initialize");
	CHECK(v34_data_carrier_init(
		      &carrier, &limited_profile, &fixture.geometry,
		      &fixture.normalizer, 8000) == V34_DATA_CARRIER_OK &&
	      v34_data_carrier_is_valid(&carrier),
	      "Phase 2 projection incorrectly limited Phase 4 carrier");
	memset(&carrier, 0xa5, sizeof(carrier));
	CHECK(v34_data_carrier_init(
		      NULL, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 8000) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_init(
		      &carrier, NULL, &fixture.geometry, &fixture.normalizer,
		      8000) == V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_init(
		      &carrier, &fixture.profile, &other_geometry,
		      &fixture.normalizer, 8000) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 7000) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      bytes_are_filled(&carrier, sizeof(carrier), 0xa5),
	      "rejected carrier initialization changed its destination");
	CHECK(v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 8000) == V34_DATA_CARRIER_OK,
	      "valid carrier did not initialize");
	carrier_before = carrier;
	excessive = (struct qam_point){
		.in_phase =
			sqrt(fixture.normalizer.maximum_normalized_point_power) *
			1.01,
	};
	CHECK(!v34_data_carrier_is_valid(NULL) &&
	      !v34_data_carrier_needs_point(NULL) &&
	      v34_data_carrier_sample(
		      NULL, &point, &carrier_sample, &accepted) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_sample(
		      &carrier, NULL, &carrier_sample, &accepted) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_sample(
		      &carrier, &excessive, &carrier_sample, &accepted) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_sample(
		      &carrier, &point, NULL, &accepted) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_sample(
		      &carrier, &point, &carrier_sample, NULL) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      memcmp(&carrier, &carrier_before, sizeof(carrier)) == 0 &&
	      carrier_sample == 123.0 && accepted,
	      "invalid carrier point changed state or output");
	CHECK(v34_data_carrier_sample(
		      &carrier, &point, &carrier_sample, &accepted) ==
		      V34_DATA_CARRIER_OK && accepted &&
	      !v34_data_carrier_needs_point(&carrier),
	      "valid carrier point was not accepted");
	carrier_before = carrier;
	CHECK(v34_data_carrier_sample(
		      &carrier, &point, &carrier_sample, &accepted) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      memcmp(&carrier, &carrier_before, sizeof(carrier)) == 0,
	      "carrier accepted a point between boundaries");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "carrier point stream", test_carrier_point_stream },
		{ "all carrier profiles", test_all_carrier_profiles },
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
