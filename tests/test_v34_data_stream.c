// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_stream.h>
#include <v34_power_calibration.h>

#include "test.h"
#include "v34_state_fixture.h"
#include "v34_stream_fixture.h"
#include "v34_mapping_fixture.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct stream_configuration {
	struct v34_phase3_profile profile;
	struct v34_data_direction_parameters direction;
	uint32_t sample_rate;
	int16_t nominal_peak_amplitude;
	bool auxiliary_channel_enabled;
};

static unsigned int line_calibrations;

enum v34_power_calibration_result __real_v34_calibrate_line_power(
	const struct v34_frame_schedule *, const struct v34_scrambler *,
	const struct v34_4d_encoder *, const struct v34_power_normalizer *,
	const struct v34_phase3_profile *, uint32_t,
	struct v34_line_power_measurement *);

enum v34_power_calibration_result __wrap_v34_calibrate_line_power(
	const struct v34_frame_schedule *schedule,
	const struct v34_scrambler *scrambler,
	const struct v34_4d_encoder *encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile, uint32_t sample_rate,
	struct v34_line_power_measurement *measurement)
{
	line_calibrations++;
	return __real_v34_calibrate_line_power(schedule, scrambler, encoder,
					     normalizer, profile, sample_rate,
					     measurement);
}

static struct stream_configuration
configuration_fixture(void)
{
	return (struct stream_configuration){
		.profile = {
			.symbol_rate_numerator = 3000,
			.symbol_rate_denominator = 1,
			.carrier_frequency_numerator = 2000,
			.carrier_frequency_denominator = 1,
			.symbol_rate = V34_SYMBOL_RATE_3000,
			.local_role = V34_CALLING_STATION,
			.transmit_power_reduction_db = 2,
			.preemphasis_filter_index = 3,
			.projected_data_rate_2400_bps_units = 8,
			.high_carrier = true,
		},
		.direction = {
			.precoder_coefficients = {
				{ 4096, 2048 },
				{ -2048, 1024 },
				{ 1024, -2048 },
			},
			.trellis_selection = V34_TRELLIS_32_STATE,
			.nonlinear_parameter = V34_NONLINEAR_THETA_5_OVER_16,
			.primary_data_rate_2400_bps_units = 8,
		},
		.sample_rate = 8000,
		.nominal_peak_amplitude = 5000,
		.auxiliary_channel_enabled = true,
	};
}

static bool prepare_mapping(
	const struct stream_configuration *configuration,
	struct v34_data_mapping *mapping)
{
	return v34_prepare_mapping_fixture(
		       mapping, &configuration->profile, &configuration->direction,
		       configuration->auxiliary_channel_enabled) ==
	       V34_DATA_MAPPING_OK;
}

static bool initialize_stream(
	struct v34_data_stream *stream,
	const struct stream_configuration *configuration)
{
	struct v34_training_line line;
	struct v34_data_stream_preparation preparation;

	return configuration != NULL &&
	       v34_training_line_init(
		       &line, &configuration->profile,
		       configuration->sample_rate,
		       configuration->nominal_peak_amplitude) ==
		       V34_TRAINING_LINE_OK &&
	       v34_prepare_stream_fixture(
		       &preparation, &configuration->profile,
		       &configuration->direction,
		       configuration->auxiliary_channel_enabled,
		       configuration->sample_rate) == V34_DATA_STREAM_OK &&
	       v34_data_stream_continue_prepared_b1(
		       stream, &line, &preparation) == V34_DATA_STREAM_OK;
}

static void fill_generated_frame(
	const struct v34_mapping_frame *request, size_t frame_index,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	uint32_t state = UINT32_C(0x7a59d31b) ^ (uint32_t)frame_index;

	for (size_t index = 0; index < request->primary_bits; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		primary_bits[index] = (uint8_t)(state >> 31);
	}
	if (auxiliary_bit != NULL)
		*auxiliary_bit = (uint8_t)(frame_index & 1u);
}

enum source_mode {
	SOURCE_GENERATED,
	SOURCE_FAIL,
	SOURCE_MALFORMED,
};

struct source_fixture {
	size_t calls;
	enum source_mode mode;
};

static bool generated_source(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct source_fixture *source = context;

	if (source == NULL || request == NULL || primary_bits == NULL)
		return false;
	if (source->mode == SOURCE_FAIL) {
		source->calls++;
		return false;
	}
	fill_generated_frame(
		request, source->calls, primary_bits, auxiliary_bit);
	source->calls++;
	if (source->mode == SOURCE_MALFORMED)
		primary_bits[request->primary_bits - 1u] = 2;
	return true;
}

static bool test_b1_and_host_handoff(void)
{
	struct stream_configuration configuration =
		configuration_fixture();
	struct v34_data_stream stream;
	struct v34_data_points model_points;
	struct v34_training_line model_line;
	struct source_fixture source = { 0 };
	uint32_t model_delayed_primary_bits;
	uint8_t model_delay_bits;
	bool model_b1 = true;
	size_t samples = 0;

	CHECK(initialize_stream(&stream, &configuration) &&
	      v34_data_stream_is_transmitting_b1(&stream),
	      "B1 stream did not initialize");
	model_points = stream.points;
	model_line = stream.line;
	model_delay_bits = stream.primary_delay_bits;
	model_delayed_primary_bits =
		UINT32_MAX >> (32u - model_delay_bits);
	while (source.calls < 3u) {
		int16_t expected_sample;
		int16_t actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		CHECK(samples++ < 4096u, "B1 handoff did not complete");
		if (v34_data_points_needs_mapping_frame(&model_points)) {
			struct v34_mapping_frame request;
			uint8_t primary_bits[
				V34_MAX_MAPPING_FRAME_BITS];
			uint8_t sourced_primary_bits[
				V34_MAX_MAPPING_FRAME_BITS];
			uint8_t auxiliary_bit = 1;
			const uint8_t *auxiliary;

			if (model_b1 &&
			    v34_frame_schedule_is_complete(
				    &model_points.schedule))
				model_b1 = false;
			CHECK(v34_data_points_preview_mapping_frame(
				      &model_points, &request) ==
				      V34_DATA_POINTS_OK,
			      "model frame preview failed");
			if (model_b1) {
				memset(sourced_primary_bits, 1,
				       request.primary_bits);
			} else {
				fill_generated_frame(
					&request, source.calls,
					sourced_primary_bits,
					request.carries_auxiliary_bit ?
						&auxiliary_bit : NULL);
			}
			for (size_t bit = 0; bit < request.primary_bits; ++bit) {
				primary_bits[bit] =
					(uint8_t)(model_delayed_primary_bits & 1u);
				model_delayed_primary_bits =
					(model_delayed_primary_bits >> 1) |
					((uint32_t)sourced_primary_bits[bit] <<
					 (model_delay_bits - 1u));
			}
			auxiliary = request.carries_auxiliary_bit ?
				&auxiliary_bit : NULL;
			CHECK(v34_data_points_queue_mapping_frame(
				      &model_points, primary_bits,
				      request.primary_bits,
				      auxiliary) ==
				      V34_DATA_POINTS_OK,
			      "model frame queue failed");
		}
		{
			struct qam_point point;
			const struct qam_point *next_point = NULL;

			if (v34_training_line_needs_point(&model_line)) {
				CHECK(v34_data_points_next(
					      &model_points, &point) ==
					      V34_DATA_POINTS_OK,
				      "model point generation failed");
				next_point = &point;
			}
			CHECK(v34_training_line_sample(
				      &model_line, next_point, &expected_sample,
				      &expected_accepted) ==
				      V34_TRAINING_LINE_OK,
			      "model line generation failed");
		}
		CHECK(
		      v34_data_stream_sample(
			      &stream, generated_source, &source, &actual_sample,
			      &actual_accepted) ==
			      V34_DATA_STREAM_OK &&
		      actual_sample == expected_sample &&
		      actual_accepted == expected_accepted &&
		      stream.b1_active == model_b1 &&
		      stream.delayed_primary_bits ==
			      model_delayed_primary_bits &&
		      stream.primary_delay_bits == model_delay_bits &&
		      v34_data_points_equal(&stream.points, &model_points) &&
		      v34_training_lines_equal(&stream.line, &model_line),
		      "stream diverged at sample %zu", samples - 1u);
	}
	CHECK(!v34_data_stream_is_transmitting_b1(&stream) &&
	      stream.points.schedule.data_frame_in_superframe == 0 &&
	      samples > 0,
	      "host source did not begin after B1");
	return true;
}

static bool advance_to_b1_handoff(
	struct v34_data_stream *stream)
{
	for (size_t samples = 0; samples < 4096u; ++samples) {
		int16_t sample;
		bool accepted;

		if (stream->b1_active &&
		    v34_frame_schedule_is_complete(
			    &stream->points.schedule) &&
		    v34_data_points_needs_mapping_frame(&stream->points))
			return true;
		if (v34_data_stream_sample(
			    stream, NULL, NULL, &sample, &accepted) !=
		    V34_DATA_STREAM_OK)
			return false;
	}
	return false;
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

static bool streams_match_through_b1(
	struct v34_data_stream *left,
	struct v34_data_stream *right)
{
	for (size_t sample_index = 0; sample_index < 1024u; ++sample_index) {
		int16_t left_sample;
		int16_t right_sample;
		bool left_accepted;
		bool right_accepted;

		if (v34_data_stream_sample(
			    left, NULL, NULL, &left_sample, &left_accepted) !=
			    V34_DATA_STREAM_OK ||
		    v34_data_stream_sample(
			    right, NULL, NULL, &right_sample, &right_accepted) !=
			    V34_DATA_STREAM_OK ||
		    left_sample != right_sample ||
		    left_accepted != right_accepted ||
		    left->b1_active != right->b1_active)
			return false;
		if (!left->b1_active)
			return true;
	}
	return false;
}

/* Independent synchronous reference for the bounded production calibration. */
static bool prepare_reference(
	struct v34_data_stream_preparation *preparation,
	const struct v34_data_mapping *mapping, uint32_t sample_rate)
{
	struct v34_line_power_measurement measurement;

	if (v34_calibrate_line_power(&mapping->schedule, &mapping->scrambler,
		    &mapping->encoder, &mapping->normalizer,
		    &mapping->transmitting_profile, sample_rate, &measurement) !=
	    V34_POWER_CALIBRATION_OK)
		return false;
	*preparation = (struct v34_data_stream_preparation){
		.mapping = *mapping,
		.line_peak = measurement.peak_amplitude,
		.sample_rate = sample_rate,
	};
	return true;
}

static bool initialization_is_rejected(
	const struct stream_configuration *configuration,
	enum v34_data_stream_result expected)
{
	struct v34_data_stream_preparation_job job;

	memset(&job, 0xa5, sizeof(job));
	return v34_data_stream_begin_preparation(&job, &configuration->profile,
		       &configuration->direction,
		       configuration->auxiliary_channel_enabled,
		       configuration->sample_rate) == expected &&
	       bytes_are_filled(&job, sizeof(job), 0xa5);
}

static bool test_selected_continuation(void)
{
	for (unsigned int role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role) {
		struct stream_configuration configuration =
			configuration_fixture();
		struct v34_data_mapping mapping;
		struct v34_data_stream_preparation preparation, incremental;
		struct v34_training_line line;
		struct v34_training_line expected_line;
		struct v34_data_stream expected;
		struct v34_data_stream actual;

		configuration.profile.local_role =
			(enum v34_station_role)role;
		line_calibrations = 0;
		CHECK(prepare_mapping(&configuration, &mapping) &&
		      prepare_reference(&preparation, &mapping,
			      configuration.sample_rate) &&
		      v34_training_line_init(
			      &line, &configuration.profile,
			      configuration.sample_rate,
			      configuration.nominal_peak_amplitude) ==
			      V34_TRAINING_LINE_OK,
		      "continuation inputs failed for role %u", role);
		CHECK(line_calibrations == 1,
		      "preparation did not calibrate exactly once");
		/* Preparation must not capture history: keep transmitting training
		 * after it is ready, then attach the live carrier/filter state. */
		for (unsigned int sample_index = 0; sample_index < 137; ++sample_index) {
			const struct qam_point point = { 0.6, 0.2 };
			int16_t sample;
			bool accepted;

			CHECK(v34_training_line_sample(&line,
				      v34_training_line_needs_point(&line) ? &point : NULL,
				      &sample, &accepted) ==
				      V34_TRAINING_LINE_OK,
			      "training continuation failed");
		}
		expected_line = line;
		if (expected_line.carrier.maximum_point_power <
		    mapping.normalizer.maximum_normalized_point_power)
			expected_line.carrier.maximum_point_power =
				mapping.normalizer.maximum_normalized_point_power;
		CHECK(
		      v34_data_stream_continue_prepared_b1(
			      &expected, &line, &preparation) ==
			      V34_DATA_STREAM_OK &&
		      line_calibrations == 1 &&
		      v34_prepare_stream_fixture(
			      &incremental, &configuration.profile,
			      &configuration.direction,
			      configuration.auxiliary_channel_enabled,
			      configuration.sample_rate) ==
			      V34_DATA_STREAM_OK &&
		      v34_data_stream_continue_prepared_b1(
			      &actual, &line, &incremental) == V34_DATA_STREAM_OK &&
		      line_calibrations == 1 &&
		      v34_training_lines_equal(&expected.line, &expected_line) &&
		      v34_training_lines_equal(&actual.line, &expected_line) &&
		      v34_data_stream_is_transmitting_b1(&actual) &&
		      v34_data_streams_equal(&actual, &expected) &&
		      streams_match_through_b1(&actual, &expected),
		      "continued stream mismatch for role %u", role);
	}
	return true;
}

static bool test_preparation_boundaries(void)
{
	struct stream_configuration configuration = configuration_fixture();
	struct v34_data_mapping mapping;
	struct v34_data_stream_preparation preparation, invalid;
	struct v34_training_line line;
	struct v34_data_stream stream;
	struct v34_line_power_measurement measurement;
	const double invalid_peaks[] = { 0.0, -1.0, NAN, INFINITY };
	unsigned int clipped = 0, passed = 0;

	CHECK(prepare_mapping(&configuration, &mapping), "mapping failed");
	CHECK(!v34_data_stream_preparation_is_valid(NULL) &&
	      v34_prepare_stream_fixture(&preparation, &configuration.profile,
		      &configuration.direction,
		      configuration.auxiliary_channel_enabled,
		      configuration.sample_rate) == V34_DATA_STREAM_OK &&
	      v34_data_stream_preparation_is_valid(&preparation) &&
	      v34_calibrate_line_power(&mapping.schedule, &mapping.scrambler,
		      &mapping.encoder, &mapping.normalizer,
		      &mapping.transmitting_profile, 8000, &measurement) ==
		      V34_POWER_CALIBRATION_OK &&
	      preparation.line_peak == measurement.peak_amplitude,
	      "stored peak changed the calibration result");
	CHECK(v34_training_line_init(&line, &configuration.profile, 8000,
		      configuration.nominal_peak_amplitude) == V34_TRAINING_LINE_OK,
	      "line failed");
	memset(&stream, 0xa5, sizeof(stream));
	for (size_t index = 0; index < ARRAY_LENGTH(invalid_peaks); ++index) {
		invalid = preparation;
		invalid.line_peak = invalid_peaks[index];
		CHECK(!v34_data_stream_preparation_is_valid(&invalid) &&
		      v34_data_stream_continue_prepared_b1(&stream, &line, &invalid) ==
			      V34_DATA_STREAM_INVALID_ARGUMENT &&
		      bytes_are_filled(&stream, sizeof(stream), 0xa5),
		      "invalid peak accepted at %zu", index);
	}
	invalid = preparation;
	invalid.sample_rate = 16000;
	CHECK(v34_data_stream_continue_prepared_b1(&stream, &line, &invalid) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&stream, sizeof(stream), 0xa5),
	      "preparation reused at a different sample rate");
	invalid.sample_rate = 0;
	CHECK(!v34_data_stream_preparation_is_valid(&invalid),
	      "zero sample rate was accepted");
	invalid = preparation;
	invalid.mapping.transmitting_profile.preemphasis_filter_index++;
	CHECK(v34_data_stream_continue_prepared_b1(&stream, &line, &invalid) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&stream, sizeof(stream), 0xa5),
	      "preparation reused for a different waveform");
	/* Both paths must retain the same gain-dependent clipping boundary. */
	for (int16_t amplitude = 1000; amplitude < 32000; amplitude += 1000) {
		enum v34_data_stream_result expected;
		enum v34_training_line_result line_result = v34_training_line_init(
			&line, &configuration.profile, 8000, amplitude);

		if (line_result == V34_TRAINING_LINE_CLIPPING_RISK)
			continue;
		CHECK(line_result == V34_TRAINING_LINE_OK, "gain fixture failed");
		expected = measurement.peak_amplitude * line.output_gain >=
			INT16_MAX + 0.5 ? V34_DATA_STREAM_CLIPPING_RISK : V34_DATA_STREAM_OK;
		memset(&stream, 0xa5, sizeof(stream));
		CHECK(v34_data_stream_continue_prepared_b1(&stream, &line, &preparation) ==
			      expected, "clipping decision changed at %d", amplitude);
		if (expected == V34_DATA_STREAM_CLIPPING_RISK) {
			clipped++;
			CHECK(bytes_are_filled(&stream, sizeof(stream), 0xa5),
			      "clipping rejection changed destination");
		} else {
			passed++;
		}
	}
	CHECK(clipped != 0 && passed != 0, "clipping boundary was not exercised");
	return true;
}

static bool initialize_delay_fixture(
	struct v34_data_stream *stream, enum v34_symbol_rate symbol_rate,
	uint8_t primary_rate)
{
	struct stream_configuration configuration =
		configuration_fixture();
	struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = primary_rate,
	};

	if (v34_phase3_profile_init(
		    &configuration.profile, V34_CALLING_STATION, symbol_rate,
		    &probe, 0, 0) != V34_PHASE3_PROFILE_OK)
		return false;
	memset(configuration.direction.precoder_coefficients, 0,
	       sizeof(configuration.direction.precoder_coefficients));
	configuration.direction.trellis_selection = V34_TRELLIS_16_STATE;
	configuration.direction.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO;
	configuration.direction.primary_data_rate_2400_bps_units = primary_rate;
	configuration.auxiliary_channel_enabled = false;
	configuration.direction.expanded_shaping = false;
	return initialize_stream(stream, &configuration);
}

static bool test_word_aligned_host_handoff(void)
{
	static const uint8_t expected_2743[] = {
		24, 20, 16, 28, 24, 20, 16, 12, 8, 4,
	};
	static const uint8_t expected_3429[] = {
		24, 20, 16, 28, 24, 20, 16, 12, 24, 20, 16, 12, 8,
	};
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t first_rate;
		const uint8_t *expected;
		size_t count;
	} cases[] = {
		{ V34_SYMBOL_RATE_2743, 2, expected_2743,
		  ARRAY_LENGTH(expected_2743) },
		{ V34_SYMBOL_RATE_3429, 2, expected_3429,
		  ARRAY_LENGTH(expected_3429) },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(cases); ++fixture) {
		for (size_t index = 0; index < cases[fixture].count; ++index) {
			struct v34_data_stream stream;
			uint8_t rate = (uint8_t)(cases[fixture].first_rate + index);
			uint8_t delay = cases[fixture].expected[index];

			CHECK(initialize_delay_fixture(
				      &stream, cases[fixture].symbol_rate, rate) &&
			      stream.primary_delay_bits == delay &&
			      stream.delayed_primary_bits ==
				      (UINT32_MAX >> (32u - delay)),
			      "word-aligned handoff changed at %zu/%u", fixture,
			      rate);
		}
	}
	return true;
}

static bool test_initialization_failures(void)
{
	struct stream_configuration configuration = configuration_fixture();
	struct v34_data_stream_preparation_job rejected;
	struct v34_data_stream stream;

	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(v34_data_stream_begin_preparation(NULL, &configuration.profile,
		      &configuration.direction,
		      configuration.auxiliary_channel_enabled, 8000) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_begin_preparation(&rejected, NULL,
		      &configuration.direction,
		      configuration.auxiliary_channel_enabled, 8000) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_begin_preparation(&rejected, &configuration.profile,
		      NULL, configuration.auxiliary_channel_enabled, 8000) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&rejected, sizeof(rejected), 0xa5),
	      "null preparation input changed the job");
	configuration.direction.primary_data_rate_2400_bps_units = 9;
	CHECK(initialize_stream(&stream, &configuration) &&
	      v34_data_stream_is_valid(&stream) &&
	      stream.points.schedule.geometry.primary_data_rate_2400_bps_units == 9,
	      "Phase 2 projection incorrectly limited the Phase 4 stream");
	configuration = configuration_fixture();
	configuration.direction.primary_data_rate_2400_bps_units = 1;
	CHECK(initialization_is_rejected(
		      &configuration, V34_DATA_STREAM_OUT_OF_RANGE),
	      "unsupported symbol-rate geometry was accepted");
	configuration = configuration_fixture();
	configuration.direction.trellis_selection = (enum v34_trellis_selection)99;
	CHECK(initialization_is_rejected(
		      &configuration, V34_DATA_STREAM_INVALID_ARGUMENT),
	      "invalid trellis selection was accepted");
	configuration = configuration_fixture();
	configuration.direction.nonlinear_parameter = (enum v34_nonlinear_parameter)99;
	CHECK(initialization_is_rejected(
		      &configuration, V34_DATA_STREAM_INVALID_ARGUMENT),
	      "invalid nonlinear selection was accepted");
	configuration = configuration_fixture();
	configuration.direction.precoder_coefficients[0].real = INT16_MIN;
	CHECK(initialization_is_rejected(
		      &configuration, V34_DATA_STREAM_INVALID_ARGUMENT),
	      "out-of-domain precoder coefficient was accepted");
	configuration = configuration_fixture();
	for (unsigned rate = 0; rate <= 16000; rate += 16000) {
		configuration.sample_rate = rate;
		CHECK(initialization_is_rejected(
			      &configuration, V34_DATA_STREAM_INVALID_ARGUMENT),
		      "unsupported sample rate %u was accepted", rate);
	}
	return true;
}

static bool test_transactional_boundaries(void)
{
	struct stream_configuration configuration =
		configuration_fixture();
	struct v34_data_mapping mapping;
	struct v34_data_stream_preparation preparation;
	struct v34_data_stream_preparation wrong_position;
	struct v34_training_line line;
	struct v34_data_stream stream;
	struct v34_data_stream rejected;
	struct v34_data_stream before;
	struct v34_data_stream corrupt;
	struct source_fixture source = { 0, SOURCE_FAIL };
	int16_t sample = 1234;
	bool accepted = true;

	CHECK(prepare_mapping(&configuration, &mapping) &&
	      prepare_reference(&preparation, &mapping,
		      configuration.sample_rate) &&
	      v34_training_line_init(
		      &line, &configuration.profile, configuration.sample_rate,
		      configuration.nominal_peak_amplitude) ==
		      V34_TRAINING_LINE_OK,
	      "validation inputs did not initialize");
	wrong_position = preparation;
	wrong_position.mapping.schedule.data_frame_in_superframe = 0;
	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(v34_data_stream_continue_prepared_b1(
		      NULL, &line, &preparation) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_continue_prepared_b1(
		      &rejected, NULL, &preparation) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_continue_prepared_b1(
		      &rejected, &line, NULL) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_continue_prepared_b1(
		      &rejected, &line, &wrong_position) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&rejected, sizeof(rejected), 0xa5) &&
	      v34_data_stream_continue_prepared_b1(
		      &stream, &line, &preparation) ==
		      V34_DATA_STREAM_OK &&
	      advance_to_b1_handoff(&stream),
	      "B1 validation boundary was not reached");
	before = stream;
	CHECK(v34_data_stream_sample(
		      &stream, generated_source, &source, &sample, &accepted) ==
		      V34_DATA_STREAM_SOURCE_FAILURE &&
	      source.calls == 1u && v34_data_streams_equal(&stream, &before) &&
	      sample == 1234 && accepted,
	      "failed source changed stream state or output");
	source.mode = SOURCE_MALFORMED;
	CHECK(v34_data_stream_sample(
		      &stream, generated_source, &source, &sample, &accepted) ==
		      V34_DATA_STREAM_SOURCE_FAILURE &&
	      source.calls == 2u && v34_data_streams_equal(&stream, &before) &&
	      sample == 1234 && accepted,
	      "malformed source changed stream state or output");
	CHECK(v34_data_stream_sample(
		      &stream, NULL, NULL, &sample, &accepted) ==
		      V34_DATA_STREAM_OK &&
	      !stream.b1_active && source.calls == 2u,
	      "idle ones did not cross the B1 handoff");
	before = stream;
	CHECK(!v34_data_stream_is_valid(NULL) &&
	      !v34_data_stream_is_transmitting_b1(NULL) &&
	      v34_data_stream_sample(
		      NULL, NULL, NULL, &sample, &accepted) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_sample(
		      &stream, NULL, NULL, NULL, &accepted) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_sample(
		      &stream, NULL, NULL, &sample, NULL) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_streams_equal(&stream, &before),
	      "invalid stream call changed state");
	corrupt = stream;
	corrupt.line.carrier.maximum_point_power =
		corrupt.points.normalizer.maximum_normalized_point_power * 0.5;
	CHECK(!v34_data_stream_is_valid(&corrupt),
	      "undersized point capacity was accepted");
	stream.b1_active = true;
	CHECK(!v34_data_stream_is_valid(&stream),
	      "B1 state accepted an ordinary data frame");
	return true;
}

static bool test_incremental_preparation(void)
{
	static const uint32_t BUDGETS[] = { 1, 23, UINT32_MAX };
	for (unsigned int nonlinear = 0; nonlinear < 2; ++nonlinear) {
		struct stream_configuration config = configuration_fixture();
		struct v34_data_mapping mapping;
		struct v34_data_stream_preparation expected;
		struct v34_training_line line;
		config.direction.nonlinear_parameter = nonlinear ? V34_NONLINEAR_THETA_5_OVER_16 :
			V34_NONLINEAR_THETA_ZERO;
		CHECK(prepare_mapping(&config, &mapping) &&
		      prepare_reference(&expected, &mapping, 8000) &&
		      v34_training_line_init(&line, &config.profile, 8000, config.nominal_peak_amplitude) ==
		      V34_TRAINING_LINE_OK, "shared preparation fixture");
		for (size_t trial = 0; trial < ARRAY_LENGTH(BUDGETS); ++trial) {
			struct v34_data_stream_preparation_job job;
			struct v34_data_stream_preparation prepared;
			struct v34_data_stream a, b;
			enum v34_data_stream_result result;
			unsigned int steps = 0;
			CHECK(v34_data_stream_begin_preparation(&job, &config.profile, &config.direction,
				config.auxiliary_channel_enabled, 8000) == V34_DATA_STREAM_OK,
			      "shared preparation begin");
			memset(&prepared, 0xa5, sizeof(prepared));
			CHECK(v34_data_stream_step_preparation(NULL, 1, &prepared) ==
			      V34_DATA_STREAM_INVALID_ARGUMENT &&
			      v34_data_stream_step_preparation(&job, 1, NULL) ==
			      V34_DATA_STREAM_INVALID_ARGUMENT &&
			      bytes_are_filled(&prepared, sizeof(prepared), 0xa5),
			      "null preparation step changed output");
			CHECK(v34_data_stream_step_preparation(&job, 0, &prepared) ==
			      V34_DATA_STREAM_INVALID_ARGUMENT && bytes_are_filled(&prepared, sizeof(prepared), 0xa5),
			      "zero budget changed output");
			CHECK(v34_data_stream_step_preparation(&job, 1, &job.preparation) ==
			      V34_DATA_STREAM_INVALID_ARGUMENT, "active job aliases its output");
			do {
				result = v34_data_stream_step_preparation(&job, BUDGETS[trial], &prepared);
				CHECK(result == V34_DATA_STREAM_OK || result == V34_DATA_STREAM_IN_PROGRESS,
				      "shared preparation step failed");
				if (result == V34_DATA_STREAM_IN_PROGRESS)
					CHECK(bytes_are_filled(&prepared, sizeof(prepared), 0xa5), "partial output escaped");
				CHECK(++steps < 2000, "shared preparation did not complete");
			} while (result == V34_DATA_STREAM_IN_PROGRESS);
			CHECK(prepared.line_peak == expected.line_peak &&
			      prepared.mapping.normalizer.precoded_mean_squared_magnitude ==
				expected.mapping.normalizer.precoded_mean_squared_magnitude &&
			      prepared.mapping.normalizer.projected_mean_squared_magnitude ==
				expected.mapping.normalizer.projected_mean_squared_magnitude &&
			      v34_data_stream_continue_prepared_b1(&a, &line, &expected) == V34_DATA_STREAM_OK &&
			      v34_data_stream_continue_prepared_b1(&b, &line, &prepared) == V34_DATA_STREAM_OK,
			      "shared power preparation differs");
			CHECK(v34_data_streams_equal(&a, &b),
			      "bounded preparation changed named stream state");
			for (unsigned int sample = 0; sample < 1000; ++sample) {
				int16_t left, right;
				bool accepted_left, accepted_right;
				CHECK(v34_data_stream_sample(&a, NULL, NULL, &left, &accepted_left) == V34_DATA_STREAM_OK &&
				      v34_data_stream_sample(&b, NULL, NULL, &right, &accepted_right) == V34_DATA_STREAM_OK &&
				      left == right && accepted_left == accepted_right, "shared waveform differs");
			}
			CHECK(v34_data_stream_step_preparation(&job, 1, &job.preparation) == V34_DATA_STREAM_OK,
			      "completed job cannot return its owned preparation");
		}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "incremental preparation", test_incremental_preparation },
		{ "selected continuation", test_selected_continuation },
		{ "preparation boundaries", test_preparation_boundaries },
		{ "word-aligned host handoff", test_word_aligned_host_handoff },
		{ "initialization failures", test_initialization_failures },
		{ "B1 and host handoff", test_b1_and_host_handoff },
		{ "transactional boundaries", test_transactional_boundaries },
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
