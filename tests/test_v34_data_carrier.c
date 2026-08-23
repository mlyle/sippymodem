// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_line_transmitter.h>
#include <v34_data_points.h>

#include "test.h"

#include <float.h>
#include <limits.h>
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

static bool test_active_carrier_handoff(void)
{
	static const struct qam_point TRAINING_POINT = { 1.0, 0.0 };
	const double point_gain = 1.25;
	struct carrier_fixture fixture;
	struct v34_data_points points;
	struct qam_channel channel;
	struct v34_pulse_modulator active;
	struct v34_pulse_modulator active_before;
	struct v34_pulse_modulator model;
	struct v34_data_carrier carrier;
	struct qam_point point;
	double ignored;
	double required_power;
	bool ignored_accepted;
	bool data_point_accepted = false;
	bool retained_pulse_seen = false;

	CHECK(initialize_fixture(&fixture) &&
	      v34_data_carrier_channel(
		      &fixture.profile, 8000, &channel) == V34_DATA_CARRIER_OK &&
	      v34_pulse_modulator_init(
		      &active, &fixture.profile, &channel, INT16_MAX,
		      fixture.normalizer.maximum_normalized_point_power) ==
		      V34_PULSE_MODULATOR_OK &&
	      v34_pulse_modulator_sample(
		      &active, &TRAINING_POINT, &ignored,
		      &ignored_accepted) == V34_PULSE_MODULATOR_OK &&
	      ignored_accepted && active.generated_samples == 1,
	      "active-carrier fixture did not retain its training pulse");
	active_before = active;
	required_power = fixture.normalizer.maximum_normalized_point_power *
		point_gain * point_gain;
	CHECK(required_power > active.maximum_point_power &&
	      v34_data_carrier_continue(
		      &carrier, &active, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, point_gain) == V34_DATA_CARRIER_OK &&
	      memcmp(&active, &active_before, sizeof(active)) == 0 &&
	      carrier.point_gain == point_gain &&
	      carrier.modulator.maximum_point_power == required_power &&
	      carrier.modulator.generated_samples == active.generated_samples &&
	      memcmp(carrier.modulator.history, active.history,
		     sizeof(active.history)) == 0 &&
	      carrier.modulator.carrier_phase == active.carrier_phase &&
	      carrier.modulator.symbol_phase_units ==
		      active.symbol_phase_units,
	      "data continuation reset or changed the active carrier");
	points = fixture.points;
	model = carrier.modulator;
	for (size_t sample_index = 0;
	     sample_index < 16u && !data_point_accepted; ++sample_index) {
		struct qam_point scaled;
		const struct qam_point *point_pointer = NULL;
		const struct qam_point *model_pointer = NULL;
		double expected;
		double actual;
		bool model_accepted;
		bool actual_accepted;

		if (v34_data_carrier_needs_point(&carrier)) {
			CHECK(v34_data_points_next(&points, &point) ==
				      V34_DATA_POINTS_OK,
			      "continued carrier did not obtain a data point");
			point_pointer = &point;
			scaled = (struct qam_point){
				.in_phase = point.in_phase * point_gain,
				.quadrature = point.quadrature * point_gain,
			};
			model_pointer = &scaled;
		}
		CHECK(v34_pulse_modulator_sample(
			      &model, model_pointer, &expected,
			      &model_accepted) == V34_PULSE_MODULATOR_OK &&
		      v34_data_carrier_sample(
			      &carrier, point_pointer, &actual,
			      &actual_accepted) == V34_DATA_CARRIER_OK &&
		      actual == expected &&
		      actual_accepted == model_accepted &&
		      memcmp(&carrier.modulator, &model, sizeof(model)) == 0,
		      "continued carrier diverged at sample %zu", sample_index);
		retained_pulse_seen |= point_pointer == NULL && actual != 0.0;
		data_point_accepted |= actual_accepted;
	}
	CHECK(retained_pulse_seen && data_point_accepted,
	      "continued carrier lost its pulse tail or next data point");
	return true;
}

static bool test_line_output(void)
{
	struct carrier_fixture fixture;
	struct v34_data_points points;
	struct v34_data_line_transmitter line;
	struct v34_data_carrier model_carrier;
	struct v34_preemphasis model_filter;
	size_t samples = 0;
	bool nonzero_seen = false;

	CHECK(initialize_fixture(&fixture) &&
	      v34_data_line_transmitter_init(
		      &line, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == V34_DATA_LINE_TRANSMITTER_OK,
	      "data line did not initialize");
	points = fixture.points;
	model_carrier = line.carrier;
	model_filter = line.preemphasis;
	while (!v34_data_points_needs_mapping_frame(&points)) {
		struct qam_point point;
		const struct qam_point *point_pointer = NULL;
		double carrier_sample;
		double filtered_sample;
		long expected_sample;
		int16_t actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		if (v34_data_line_transmitter_needs_point(&line)) {
			CHECK(v34_data_points_next(&points, &point) ==
				      V34_DATA_POINTS_OK,
			      "line point stream failed at sample %zu", samples);
			point_pointer = &point;
		}
		CHECK(samples++ < 64u &&
		      v34_data_carrier_sample(
			      &model_carrier, point_pointer, &carrier_sample,
			      &expected_accepted) == V34_DATA_CARRIER_OK &&
		      v34_preemphasis_sample(
			      &model_filter, carrier_sample, &filtered_sample) ==
			      V34_PREEMPHASIS_OK,
		      "line model failed at sample %zu", samples - 1u);
		expected_sample = lround(filtered_sample * line.output_gain);
		CHECK(v34_data_line_transmitter_sample(
			      &line, point_pointer, &actual_sample,
			      &actual_accepted) == V34_DATA_LINE_TRANSMITTER_OK &&
		      actual_sample == expected_sample &&
		      actual_accepted == expected_accepted,
		      "line output diverged at sample %zu", samples - 1u);
		nonzero_seen |= actual_sample != 0;
	}
	CHECK(line.generated_samples == samples && nonzero_seen &&
	      memcmp(&line.carrier, &model_carrier,
		     sizeof(model_carrier)) == 0 &&
	      memcmp(&line.preemphasis, &model_filter,
		     sizeof(model_filter)) == 0,
	      "data line did not preserve its composed state");
	return true;
}

static bool advance_training_line(struct v34_training_line *line)
{
	uint32_t accepted_points = 0;

	for (size_t sample_index = 0; sample_index < 128u; ++sample_index) {
		struct qam_point point =
			v34_rotated_point_zero((int)(accepted_points & 3u));
		const struct qam_point *point_pointer =
			v34_training_line_needs_point(line) ? &point : NULL;
		int16_t ignored;
		bool accepted;

		if (v34_training_line_sample(
			    line, point_pointer, &ignored, &accepted) !=
			    V34_TRAINING_LINE_OK ||
		    accepted != (point_pointer != NULL))
			return false;
		accepted_points += accepted;
	}
	if (v34_training_line_needs_point(line)) {
		struct qam_point point =
			v34_rotated_point_zero((int)(accepted_points & 3u));
		int16_t ignored;
		bool accepted;

		if (v34_training_line_sample(
			    line, &point, &ignored, &accepted) !=
			    V34_TRAINING_LINE_OK ||
		    !accepted)
			return false;
	}
	return !v34_training_line_needs_point(line);
}

static bool test_active_line_handoff(void)
{
	struct carrier_fixture fixture;
	struct v34_training_line training;
	struct v34_training_line training_before;
	struct v34_data_line_transmitter standalone;
	struct v34_data_line_transmitter continued;
	struct v34_data_points points;
	struct v34_pulse_modulator model_carrier;
	struct v34_preemphasis model_filter;
	double independent_gain_ratio;
	bool retained_tail_seen = false;
	bool data_point_accepted = false;

	CHECK(initialize_fixture(&fixture) &&
	      v34_training_line_init(
		      &training, &fixture.profile, 8000, 4000) ==
		      V34_TRAINING_LINE_OK &&
	      advance_training_line(&training) &&
	      v34_data_line_transmitter_init(
		      &standalone, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == V34_DATA_LINE_TRANSMITTER_OK,
	      "active-line fixture did not initialize");
	training_before = training;
	independent_gain_ratio = standalone.output_gain / training.output_gain;
	CHECK(independent_gain_ratio > 0.0 &&
	      fabs(independent_gain_ratio - 1.0) > 0.01 &&
	      v34_data_line_transmitter_continue(
		      &continued, &training, &fixture.schedule,
		      &fixture.scrambler, &fixture.encoder,
		      &fixture.normalizer) == V34_DATA_LINE_TRANSMITTER_OK &&
	      memcmp(&training, &training_before, sizeof(training)) == 0 &&
	      continued.output_gain == training.output_gain &&
	      continued.gain_reference_rms == training.raw_rms &&
	      continued.carrier.point_gain == 1.0 &&
	      fabs(continued.output_gain * continued.gain_reference_rms -
		   continued.target_rms) < 1e-12 &&
	      continued.target_rms == training.target_rms &&
	      continued.generated_samples == training.generated_samples &&
	      memcmp(&continued.preemphasis, &training.preemphasis,
		     sizeof(training.preemphasis)) == 0 &&
	      continued.carrier.modulator.carrier_phase ==
		      training.carrier.carrier_phase &&
	      continued.carrier.modulator.symbol_phase_units ==
		      training.carrier.symbol_phase_units &&
	      continued.carrier.modulator.generated_samples ==
		      training.carrier.generated_samples &&
	      memcmp(continued.carrier.modulator.history,
		     training.carrier.history,
		     sizeof(training.carrier.history)) == 0,
	      "data line did not preserve the active training waveform");
	points = fixture.points;
	model_carrier = continued.carrier.modulator;
	model_filter = training.preemphasis;
	for (size_t sample_index = 0;
	     sample_index < 32u && !data_point_accepted; ++sample_index) {
		struct qam_point point;
		const struct qam_point *point_pointer = NULL;
		const struct qam_point *model_pointer = NULL;
		double carrier_sample;
		double filtered_sample;
		long expected_sample;
		int16_t actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		if (v34_data_line_transmitter_needs_point(&continued)) {
			CHECK(v34_data_points_next(&points, &point) ==
				      V34_DATA_POINTS_OK,
			      "continued line did not obtain a data point");
			point_pointer = &point;
			model_pointer = &point;
		}
		CHECK(v34_pulse_modulator_sample(
			      &model_carrier, model_pointer, &carrier_sample,
			      &expected_accepted) == V34_PULSE_MODULATOR_OK &&
		      v34_preemphasis_sample(
			      &model_filter, carrier_sample, &filtered_sample) ==
			      V34_PREEMPHASIS_OK,
		      "continued line model failed at sample %zu", sample_index);
		expected_sample = lround(filtered_sample * training.output_gain);
		CHECK(expected_sample >= INT16_MIN &&
		      expected_sample <= INT16_MAX &&
		      v34_data_line_transmitter_sample(
			      &continued, point_pointer, &actual_sample,
			      &actual_accepted) == V34_DATA_LINE_TRANSMITTER_OK &&
		      actual_sample == expected_sample &&
		      actual_accepted == expected_accepted &&
		      memcmp(&continued.carrier.modulator, &model_carrier,
			     sizeof(model_carrier)) == 0 &&
		      memcmp(&continued.preemphasis, &model_filter,
			     sizeof(model_filter)) == 0,
		      "continued line diverged at sample %zu", sample_index);
		retained_tail_seen |= point_pointer == NULL && actual_sample != 0;
		data_point_accepted |= actual_accepted;
	}
	CHECK(retained_tail_seen && data_point_accepted,
	      "continued line lost its training tail or first data point");
	return true;
}

static bool test_line_power_reduction(void)
{
	struct carrier_fixture fixture;
	struct v34_phase3_profile reduced_profile;
	struct v34_data_points points;
	struct v34_data_line_transmitter full;
	struct v34_data_line_transmitter reduced;
	long double full_squares = 0.0L;
	long double reduced_squares = 0.0L;
	double expected_ratio = pow(10.0, -6.0 / 20.0);
	size_t samples = 0;

	CHECK(initialize_fixture(&fixture),
	      "power-reduction fixture did not initialize");
	reduced_profile = fixture.profile;
	reduced_profile.transmit_power_reduction_db = 6;
	CHECK(v34_data_line_transmitter_init(
		      &full, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == V34_DATA_LINE_TRANSMITTER_OK &&
	      v34_data_line_transmitter_init(
		      &reduced, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &reduced_profile,
		      8000, 4000) == V34_DATA_LINE_TRANSMITTER_OK &&
	      memcmp(&full.calibration, &reduced.calibration,
		     sizeof(full.calibration)) == 0 &&
	      fabs(reduced.output_gain / full.output_gain - expected_ratio) <
		      1e-12,
	      "power reduction changed calibration or gain");
	points = fixture.points;
	/* Repeat a generated frame so integer quantization is measured over the
	 * steady waveform rather than only the pulse shaper's startup. */
	for (samples = 0; samples < 512u; ++samples) {
		struct qam_point point;
		const struct qam_point *point_pointer = NULL;
		int16_t full_sample;
		int16_t reduced_sample;
		bool full_accepted;
		bool reduced_accepted;

		CHECK(v34_data_line_transmitter_needs_point(&full) ==
			      v34_data_line_transmitter_needs_point(&reduced),
		      "reduced line clock diverged");
		if (v34_data_line_transmitter_needs_point(&full)) {
			if (v34_data_points_needs_mapping_frame(&points))
				points = fixture.points;
			CHECK(v34_data_points_next(&points, &point) ==
				      V34_DATA_POINTS_OK,
			      "power-reduction point stream failed");
			point_pointer = &point;
		}
		CHECK(v34_data_line_transmitter_sample(
			      &full, point_pointer, &full_sample,
			      &full_accepted) == V34_DATA_LINE_TRANSMITTER_OK &&
		      v34_data_line_transmitter_sample(
			      &reduced, point_pointer, &reduced_sample,
			      &reduced_accepted) == V34_DATA_LINE_TRANSMITTER_OK &&
		      full_accepted == reduced_accepted,
		      "reduced line output diverged at sample %zu", samples);
		full_squares += (long double)full_sample * full_sample;
		reduced_squares += (long double)reduced_sample * reduced_sample;
	}
	CHECK(fabsl(sqrtl(reduced_squares / samples) /
			    sqrtl(full_squares / samples) -
		    expected_ratio) < 0.001L,
	      "rendered data did not honor negotiated power reduction");
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
			      v34_data_carrier_channel_matches(
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
	struct v34_data_line_transmitter line;
	struct v34_data_line_transmitter line_before;
	struct v34_training_line training;
	struct v34_training_line invalid_training;
	struct v34_phase3_profile limited_profile;
	struct v34_frame_geometry other_geometry;
	struct qam_point point = { 0.1, -0.2 };
	struct qam_point excessive;
	struct v34_pulse_modulator active;
	struct qam_channel other_channel;
	struct v34_phase3_profile other_profile;
	double carrier_sample = 123.0;
	int16_t line_sample = 1234;
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
	active = carrier.modulator;
	other_channel = active.channel;
	other_channel.carrier_frequency_numerator = 1800;
	other_profile = fixture.profile;
	other_profile.carrier_frequency_numerator = 1800;
	other_profile.high_carrier = false;
	CHECK(v34_pulse_modulator_init(
		      &active, &other_profile, &other_channel, INT16_MAX,
		      fixture.normalizer.maximum_normalized_point_power) ==
		      V34_PULSE_MODULATOR_OK,
	      "mismatched active carrier did not initialize");
	carrier_before = carrier;
	CHECK(v34_data_carrier_continue(
		      NULL, &carrier.modulator, &fixture.profile,
		      &fixture.geometry, &fixture.normalizer, 1.0) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_continue(
		      &carrier, NULL, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 1.0) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_continue(
		      &carrier, &active, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 1.0) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_continue(
		      &carrier, &carrier.modulator, &fixture.profile,
		      &fixture.geometry, &fixture.normalizer, 0.0) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_continue(
		      &carrier, &carrier.modulator, &fixture.profile,
		      &fixture.geometry, &fixture.normalizer, NAN) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      v34_data_carrier_continue(
		      &carrier, &carrier.modulator, &fixture.profile,
		      &fixture.geometry, &fixture.normalizer, DBL_MAX) ==
		      V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      memcmp(&carrier, &carrier_before, sizeof(carrier)) == 0,
	      "invalid active-carrier continuation changed its destination");
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

	memset(&line, 0xa5, sizeof(line));
	CHECK(v34_data_line_transmitter_init(
		      NULL, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_init(
		      &line, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 0) == V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_init(
		      &line, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, INT16_MAX) ==
		      V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK &&
	      bytes_are_filled(&line, sizeof(line), 0xa5),
	      "rejected line initialization changed its destination");
	CHECK(v34_data_line_transmitter_init(
		      &line, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == V34_DATA_LINE_TRANSMITTER_OK,
	      "valid line did not initialize");
	line_before = line;
	line.gain_reference_rms = NAN;
	CHECK(!v34_data_line_transmitter_is_valid(&line),
	      "line accepted an invalid gain reference");
	line = line_before;
	line.carrier.point_gain = 1.01;
	CHECK(!v34_data_line_transmitter_is_valid(&line),
	      "line accepted a post-training point scale change");
	line = line_before;
	CHECK(v34_training_line_init(
		      &training, &fixture.profile, 8000, 4000) ==
		      V34_TRAINING_LINE_OK,
	      "line-continuation fixture did not initialize");
	invalid_training = training;
	invalid_training.generated_samples++;
	line_before = line;
	CHECK(!v34_data_line_transmitter_is_valid(NULL) &&
	      !v34_data_line_transmitter_needs_point(NULL) &&
	      v34_data_line_transmitter_sample(
		      NULL, &point, &line_sample, &accepted) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_sample(
		      &line, NULL, &line_sample, &accepted) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_sample(
		      &line, &point, NULL, &accepted) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_sample(
		      &line, &point, &line_sample, NULL) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&line, &line_before, sizeof(line)) == 0 &&
	      line_sample == 1234 && accepted,
	      "invalid line point changed state or output");
	CHECK(v34_data_line_transmitter_continue(
		      NULL, &training, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_continue(
		      &line, NULL, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_data_line_transmitter_continue(
		      &line, &invalid_training, &fixture.schedule,
		      &fixture.scrambler, &fixture.encoder,
		      &fixture.normalizer) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&line, &line_before, sizeof(line)) == 0,
	      "invalid line continuation changed its destination");
	CHECK(v34_data_line_transmitter_sample(
		      &line, &point, &line_sample, &accepted) ==
		      V34_DATA_LINE_TRANSMITTER_OK && accepted,
	      "valid line point was not accepted");
	line_before = line;
	CHECK(v34_data_line_transmitter_sample(
		      &line, &point, &line_sample, &accepted) ==
		      V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&line, &line_before, sizeof(line)) == 0,
	      "line accepted a point between boundaries");
	line.generated_samples = UINT64_MAX;
	line.carrier.modulator.generated_samples = UINT64_MAX;
	line.preemphasis.next_history = UINT64_MAX % V34_PREEMPHASIS_TAPS;
	line.carrier.modulator.carrier_phase =
		(uint32_t)UINT64_MAX *
		line.carrier.modulator.carrier_phase_step;
	line.carrier.modulator.symbol_phase_units =
		(UINT64_MAX % line.carrier.modulator.symbol_period_units) *
		line.carrier.profile.symbol_rate_numerator %
		line.carrier.modulator.symbol_period_units;
	line_before = line;
	CHECK(v34_data_line_transmitter_is_valid(&line) &&
	      v34_data_line_transmitter_sample(
		      &line, NULL, &line_sample, &accepted) ==
		      V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE &&
	      memcmp(&line, &line_before, sizeof(line)) == 0,
	      "exhausted line sample count changed state");
	CHECK(strcmp(v34_data_carrier_result_name(V34_DATA_CARRIER_OK),
		     "ok") == 0 &&
	      strcmp(v34_data_carrier_result_name(
		     V34_DATA_CARRIER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_data_carrier_result_name(V34_DATA_CARRIER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v34_data_carrier_result_name(
		     V34_DATA_CARRIER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_data_carrier_result_name(
		     (enum v34_data_carrier_result)99),
		     "unknown") == 0 &&
	      strcmp(v34_data_line_transmitter_result_name(
		     V34_DATA_LINE_TRANSMITTER_OK),
		     "ok") == 0 &&
	      strcmp(v34_data_line_transmitter_result_name(
		     V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_data_line_transmitter_result_name(
		     V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v34_data_line_transmitter_result_name(
		     V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v34_data_line_transmitter_result_name(
		     V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_data_line_transmitter_result_name(
		     (enum v34_data_line_transmitter_result)99),
		     "unknown") == 0,
	      "carrier or line result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "carrier point stream", test_carrier_point_stream },
		{ "active carrier handoff", test_active_carrier_handoff },
		{ "line output", test_line_output },
		{ "active line handoff", test_active_line_handoff },
		{ "line power reduction", test_line_power_reduction },
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
