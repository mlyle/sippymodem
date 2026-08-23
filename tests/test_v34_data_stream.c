// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_stream.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct v34_data_stream_configuration
configuration_fixture(void)
{
	return (struct v34_data_stream_configuration){
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
		.precoder_coefficients = {
			{ 4096, 2048 },
			{ -2048, 1024 },
			{ 1024, -2048 },
		},
		.sample_rate = 8000,
		.nominal_peak_amplitude = 5000,
		.trellis_selection = V34_TRELLIS_32_STATE,
		.nonlinear_parameter =
			V34_NONLINEAR_THETA_5_OVER_16,
		.primary_data_rate_2400_bps_units = 8,
		.auxiliary_channel_enabled = true,
	};
}

static bool compose_transmitter(
	struct v34_data_transmitter *transmitter,
	const struct v34_data_stream_configuration *configuration)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;

	return configuration != NULL &&
	       v34_phase3_profile_is_valid(
		       &configuration->profile) &&
	       v34_frame_geometry_init(
		       &geometry, configuration->profile.symbol_rate,
		       configuration->primary_data_rate_2400_bps_units,
		       configuration->auxiliary_channel_enabled) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       &parameters, &geometry,
		       configuration->expanded_shaping) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_b1_frame_schedule_init(&schedule, &geometry) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_transmit_scrambler_init(
		       &scrambler, configuration->profile.local_role) ==
		       V34_FRAMING_OK &&
	       v34_4d_encoder_init(
		       &encoder, &parameters,
		       configuration->precoder_coefficients,
		       configuration->trellis_selection) ==
		       V34_4D_ENCODER_OK &&
	       v34_calibrate_mapping_power(
		       &schedule, &scrambler, &encoder,
		       configuration->nonlinear_parameter,
		       &normalizer) == V34_POWER_CALIBRATION_OK &&
	       v34_data_transmitter_init(
		       transmitter, &schedule, &scrambler, &encoder,
		       &normalizer, &configuration->profile,
		       configuration->sample_rate,
		       configuration->nominal_peak_amplitude) ==
		       V34_DATA_TRANSMITTER_OK;
}

static bool initialize_transmitter(
	struct v34_data_transmitter *transmitter)
{
	struct v34_data_stream_configuration configuration =
		configuration_fixture();

	return compose_transmitter(transmitter, &configuration);
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
	struct v34_data_transmitter initial;
	struct v34_data_transmitter model;
	struct v34_data_stream stream;
	struct source_fixture source = { 0 };
	uint32_t model_delayed_primary_bits;
	uint8_t model_delay_bits;
	bool model_b1 = true;
	size_t samples = 0;

	CHECK(initialize_transmitter(&initial) &&
	      v34_data_stream_init_b1(&stream, &initial) ==
		      V34_DATA_STREAM_OK &&
	      v34_data_stream_is_transmitting_b1(&stream),
	      "B1 stream did not initialize");
	model = initial;
	model_delay_bits = stream.primary_delay_bits;
	model_delayed_primary_bits =
		UINT32_MAX >> (32u - model_delay_bits);
	while (source.calls < 3u) {
		int16_t expected_sample;
		int16_t actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		CHECK(samples++ < 4096u, "B1 handoff did not complete");
		if (v34_data_transmitter_needs_mapping_frame(
			    &model)) {
			struct v34_mapping_frame request;
			uint8_t primary_bits[
				V34_MAX_MAPPING_FRAME_BITS];
			uint8_t sourced_primary_bits[
				V34_MAX_MAPPING_FRAME_BITS];
			uint8_t auxiliary_bit = 1;
			const uint8_t *auxiliary;

			if (model_b1 &&
			    v34_frame_schedule_is_complete(
				    &model.points.schedule))
				model_b1 = false;
			CHECK(v34_data_transmitter_preview_mapping_frame(
				      &model, &request) ==
				      V34_DATA_TRANSMITTER_OK,
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
			CHECK(v34_data_transmitter_queue_mapping_frame(
				      &model, primary_bits, request.primary_bits,
				      auxiliary) ==
				      V34_DATA_TRANSMITTER_OK,
			      "model frame queue failed");
		}
		CHECK(v34_data_transmitter_sample(
			      &model, &expected_sample, &expected_accepted) ==
			      V34_DATA_TRANSMITTER_OK &&
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
		      memcmp(&stream.transmitter, &model, sizeof(model)) == 0,
		      "stream diverged at sample %zu", samples - 1u);
	}
	CHECK(!v34_data_stream_is_transmitting_b1(&stream) &&
	      stream.transmitter.points.schedule.data_frame_in_superframe == 0 &&
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
			    &stream->transmitter.points.schedule) &&
		    v34_data_transmitter_needs_mapping_frame(
			    &stream->transmitter))
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

static bool initialization_is_rejected(
	const struct v34_data_stream_configuration *configuration,
	enum v34_data_stream_result expected)
{
	struct v34_data_stream stream;

	memset(&stream, 0xa5, sizeof(stream));
	return v34_data_stream_init(&stream, configuration) ==
		       expected &&
	       bytes_are_filled(&stream, sizeof(stream), 0xa5);
}

static bool test_selected_initialization(void)
{
	for (unsigned int role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role) {
		struct v34_data_stream_configuration configuration =
			configuration_fixture();
		struct v34_data_transmitter transmitter;
		struct v34_data_stream expected;
		struct v34_data_stream actual;

		configuration.profile.local_role =
			(enum v34_station_role)role;
		CHECK(compose_transmitter(&transmitter, &configuration) &&
		      v34_data_stream_init_b1(
			      &expected, &transmitter) ==
			      V34_DATA_STREAM_OK &&
		      v34_data_stream_init(
			      &actual, &configuration) ==
			      V34_DATA_STREAM_OK &&
		      v34_data_stream_is_transmitting_b1(&actual) &&
		      streams_match_through_b1(&actual, &expected),
		      "selected stream mismatch for role %u", role);
	}
	return true;
}

static bool initialize_delay_fixture(
	struct v34_data_stream *stream, enum v34_symbol_rate symbol_rate,
	uint8_t primary_rate)
{
	struct v34_data_stream_configuration configuration =
		configuration_fixture();
	struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = primary_rate,
	};

	if (v34_phase3_profile_init(
		    &configuration.profile, V34_CALLING_STATION, symbol_rate,
		    &probe, 0, 0) != V34_PHASE3_PROFILE_OK)
		return false;
	memset(configuration.precoder_coefficients, 0,
	       sizeof(configuration.precoder_coefficients));
	configuration.trellis_selection = V34_TRELLIS_16_STATE;
	configuration.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO;
	configuration.primary_data_rate_2400_bps_units = primary_rate;
	configuration.auxiliary_channel_enabled = false;
	configuration.expanded_shaping = false;
	return v34_data_stream_init(stream, &configuration) ==
		V34_DATA_STREAM_OK;
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
	struct v34_data_stream_configuration configuration =
		configuration_fixture();
	struct v34_data_stream rejected;

	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(v34_data_stream_init(NULL, &configuration) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_init(&rejected, NULL) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&rejected, sizeof(rejected), 0xa5),
	      "null initialization input changed the stream");
	configuration.primary_data_rate_2400_bps_units = 9;
	CHECK(v34_data_stream_init(&rejected, &configuration) ==
		      V34_DATA_STREAM_OK &&
	      v34_data_stream_is_valid(&rejected) &&
	      rejected.transmitter.line.carrier.geometry
			      .primary_data_rate_2400_bps_units == 9,
	      "Phase 2 projection incorrectly limited the Phase 4 stream");
	configuration = configuration_fixture();
	configuration.primary_data_rate_2400_bps_units = 1;
	CHECK(initialization_is_rejected(
		      &configuration, V34_DATA_STREAM_OUT_OF_RANGE),
	      "unsupported symbol-rate geometry was accepted");
	configuration = configuration_fixture();
	configuration.trellis_selection =
		(enum v34_trellis_selection)99;
	CHECK(initialization_is_rejected(
		      &configuration,
		      V34_DATA_STREAM_INVALID_ARGUMENT),
	      "invalid trellis selection was accepted");
	configuration = configuration_fixture();
	configuration.nonlinear_parameter =
		(enum v34_nonlinear_parameter)99;
	CHECK(initialization_is_rejected(
		      &configuration,
		      V34_DATA_STREAM_INVALID_ARGUMENT),
	      "invalid nonlinear selection was accepted");
	configuration = configuration_fixture();
	configuration.precoder_coefficients[0].real = INT16_MIN;
	CHECK(initialization_is_rejected(
		      &configuration,
		      V34_DATA_STREAM_INVALID_ARGUMENT),
	      "out-of-domain precoder coefficient was accepted");
	configuration = configuration_fixture();
	configuration.sample_rate = 0;
	CHECK(initialization_is_rejected(
		      &configuration,
		      V34_DATA_STREAM_INVALID_ARGUMENT),
	      "zero sample rate was accepted");
	configuration = configuration_fixture();
	configuration.nominal_peak_amplitude = 0;
	CHECK(initialization_is_rejected(
		      &configuration,
		      V34_DATA_STREAM_INVALID_ARGUMENT),
	      "zero line amplitude was accepted");
	return true;
}

static bool test_transactional_boundaries(void)
{
	struct v34_data_transmitter transmitter;
	struct v34_data_transmitter wrong_position;
	struct v34_data_stream stream;
	struct v34_data_stream rejected;
	struct v34_data_stream before;
	struct source_fixture source = { 0, SOURCE_FAIL };
	int16_t sample = 1234;
	bool accepted = true;

	CHECK(initialize_transmitter(&transmitter),
	      "validation transmitter did not initialize");
	wrong_position = transmitter;
	wrong_position.points.schedule.data_frame_in_superframe = 0;
	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(v34_data_stream_init_b1(NULL, &transmitter) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_init_b1(&rejected, NULL) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      v34_data_stream_init_b1(
		      &rejected, &wrong_position) ==
		      V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&rejected, sizeof(rejected), 0xa5) &&
	      v34_data_stream_init_b1(&stream, &transmitter) ==
		      V34_DATA_STREAM_OK &&
	      advance_to_b1_handoff(&stream),
	      "B1 validation boundary was not reached");
	before = stream;
	CHECK(v34_data_stream_sample(
		      &stream, generated_source, &source, &sample, &accepted) ==
		      V34_DATA_STREAM_SOURCE_FAILURE &&
	      source.calls == 1u && memcmp(&stream, &before, sizeof(stream)) == 0 &&
	      sample == 1234 && accepted,
	      "failed source changed stream state or output");
	source.mode = SOURCE_MALFORMED;
	CHECK(v34_data_stream_sample(
		      &stream, generated_source, &source, &sample, &accepted) ==
		      V34_DATA_STREAM_SOURCE_FAILURE &&
	      source.calls == 2u && memcmp(&stream, &before, sizeof(stream)) == 0 &&
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
	      memcmp(&stream, &before, sizeof(stream)) == 0,
	      "invalid stream call changed state");
	stream.b1_active = true;
	CHECK(!v34_data_stream_is_valid(&stream),
	      "B1 state accepted an ordinary data frame");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "selected initialization", test_selected_initialization },
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
