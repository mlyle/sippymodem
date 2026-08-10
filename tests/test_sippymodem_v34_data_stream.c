// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_data_stream.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

static bool initialize_transmitter(
	struct sippymodem_v34_data_transmitter *transmitter)
{
	struct sippymodem_v34_phase3_profile profile = {
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_3000,
		.local_role = SIPPYMODEM_V34_CALLING_STATION,
		.transmit_power_reduction_db = 2,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_power_normalizer normalizer;

	return sippymodem_v34_phase3_profile_is_valid(&profile) &&
	       sippymodem_v34_frame_geometry_init(
		       &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, true) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	       sippymodem_v34_mapping_parameters_init(
		       &parameters, &geometry, false) ==
		       SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	       sippymodem_v34_b1_frame_schedule_init(&schedule, &geometry) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	       sippymodem_v34_transmit_scrambler_init(
		       &scrambler, SIPPYMODEM_V34_CALLING_STATION) ==
		       SIPPYMODEM_V34_FRAMING_OK &&
	       sippymodem_v34_4d_encoder_init(
		       &encoder, &parameters, COEFFICIENTS,
		       SIPPYMODEM_V34_TRELLIS_32_STATE) ==
		       SIPPYMODEM_V34_4D_ENCODER_OK &&
	       sippymodem_v34_calibrate_mapping_power(
		       &schedule, &scrambler, &encoder,
		       SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
		       &normalizer) == SIPPYMODEM_V34_POWER_CALIBRATION_OK &&
	       sippymodem_v34_data_transmitter_init(
		       transmitter, &schedule, &scrambler, &encoder,
		       &normalizer, &profile, 8000, 5000) ==
		       SIPPYMODEM_V34_DATA_TRANSMITTER_OK;
}

static void fill_generated_frame(
	const struct sippymodem_v34_mapping_frame *request, size_t frame_index,
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
	void *context, const struct sippymodem_v34_mapping_frame *request,
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
	struct sippymodem_v34_data_transmitter initial;
	struct sippymodem_v34_data_transmitter model;
	struct sippymodem_v34_data_stream stream;
	struct source_fixture source = { 0 };
	bool model_b1 = true;
	size_t samples = 0;

	CHECK(initialize_transmitter(&initial) &&
	      sippymodem_v34_data_stream_init_b1(&stream, &initial) ==
		      SIPPYMODEM_V34_DATA_STREAM_OK &&
	      sippymodem_v34_data_stream_is_transmitting_b1(&stream),
	      "B1 stream did not initialize");
	model = initial;
	while (source.calls < 3u) {
		int16_t expected_sample;
		int16_t actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		CHECK(samples++ < 4096u, "B1 handoff did not complete");
		if (sippymodem_v34_data_transmitter_needs_mapping_frame(
			    &model)) {
			struct sippymodem_v34_mapping_frame request;
			uint8_t primary_bits[
				SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
			uint8_t auxiliary_bit = 1;
			const uint8_t *auxiliary;

			if (model_b1 &&
			    sippymodem_v34_frame_schedule_is_complete(
				    &model.schedule))
				model_b1 = false;
			CHECK(sippymodem_v34_data_transmitter_preview_mapping_frame(
				      &model, &request) ==
				      SIPPYMODEM_V34_DATA_TRANSMITTER_OK,
			      "model frame preview failed");
			if (model_b1) {
				memset(primary_bits, 1, request.primary_bits);
			} else {
				fill_generated_frame(
					&request, source.calls, primary_bits,
					request.carries_auxiliary_bit ?
						&auxiliary_bit : NULL);
			}
			auxiliary = request.carries_auxiliary_bit ?
				&auxiliary_bit : NULL;
			CHECK(sippymodem_v34_data_transmitter_queue_mapping_frame(
				      &model, primary_bits, request.primary_bits,
				      auxiliary) ==
				      SIPPYMODEM_V34_DATA_TRANSMITTER_OK,
			      "model frame queue failed");
		}
		CHECK(sippymodem_v34_data_transmitter_sample(
			      &model, &expected_sample, &expected_accepted) ==
			      SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
		      sippymodem_v34_data_stream_sample(
			      &stream, generated_source, &source, &actual_sample,
			      &actual_accepted) ==
			      SIPPYMODEM_V34_DATA_STREAM_OK &&
		      actual_sample == expected_sample &&
		      actual_accepted == expected_accepted &&
		      stream.b1_active == model_b1 &&
		      memcmp(&stream.transmitter, &model, sizeof(model)) == 0,
		      "stream diverged at sample %zu", samples - 1u);
	}
	CHECK(!sippymodem_v34_data_stream_is_transmitting_b1(&stream) &&
	      stream.transmitter.schedule.data_frame_in_superframe == 0 &&
	      samples > 0,
	      "host source did not begin after B1");
	return true;
}

static bool advance_to_b1_handoff(
	struct sippymodem_v34_data_stream *stream)
{
	for (size_t samples = 0; samples < 4096u; ++samples) {
		int16_t sample;
		bool accepted;

		if (stream->b1_active &&
		    sippymodem_v34_frame_schedule_is_complete(
			    &stream->transmitter.schedule) &&
		    sippymodem_v34_data_transmitter_needs_mapping_frame(
			    &stream->transmitter))
			return true;
		if (sippymodem_v34_data_stream_sample(
			    stream, NULL, NULL, &sample, &accepted) !=
		    SIPPYMODEM_V34_DATA_STREAM_OK)
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

static bool test_transactional_boundaries(void)
{
	struct sippymodem_v34_data_transmitter transmitter;
	struct sippymodem_v34_data_transmitter wrong_position;
	struct sippymodem_v34_data_stream stream;
	struct sippymodem_v34_data_stream rejected;
	struct sippymodem_v34_data_stream before;
	struct source_fixture source = { 0, SOURCE_FAIL };
	int16_t sample = 1234;
	bool accepted = true;

	CHECK(initialize_transmitter(&transmitter),
	      "validation transmitter did not initialize");
	wrong_position = transmitter;
	wrong_position.schedule.data_frame_in_superframe = 0;
	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(sippymodem_v34_data_stream_init_b1(NULL, &transmitter) ==
		      SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT &&
	      sippymodem_v34_data_stream_init_b1(&rejected, NULL) ==
		      SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT &&
	      sippymodem_v34_data_stream_init_b1(
		      &rejected, &wrong_position) ==
		      SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT &&
	      bytes_are_filled(&rejected, sizeof(rejected), 0xa5) &&
	      sippymodem_v34_data_stream_init_b1(&stream, &transmitter) ==
		      SIPPYMODEM_V34_DATA_STREAM_OK &&
	      advance_to_b1_handoff(&stream),
	      "B1 validation boundary was not reached");
	before = stream;
	CHECK(sippymodem_v34_data_stream_sample(
		      &stream, generated_source, &source, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_STREAM_SOURCE_FAILURE &&
	      source.calls == 1u && memcmp(&stream, &before, sizeof(stream)) == 0 &&
	      sample == 1234 && accepted,
	      "failed source changed stream state or output");
	source.mode = SOURCE_MALFORMED;
	CHECK(sippymodem_v34_data_stream_sample(
		      &stream, generated_source, &source, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_STREAM_SOURCE_FAILURE &&
	      source.calls == 2u && memcmp(&stream, &before, sizeof(stream)) == 0 &&
	      sample == 1234 && accepted,
	      "malformed source changed stream state or output");
	CHECK(sippymodem_v34_data_stream_sample(
		      &stream, NULL, NULL, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_STREAM_OK &&
	      !stream.b1_active && source.calls == 2u,
	      "idle ones did not cross the B1 handoff");
	before = stream;
	CHECK(!sippymodem_v34_data_stream_is_valid(NULL) &&
	      !sippymodem_v34_data_stream_is_transmitting_b1(NULL) &&
	      sippymodem_v34_data_stream_sample(
		      NULL, NULL, NULL, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT &&
	      sippymodem_v34_data_stream_sample(
		      &stream, NULL, NULL, NULL, &accepted) ==
		      SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT &&
	      sippymodem_v34_data_stream_sample(
		      &stream, NULL, NULL, &sample, NULL) ==
		      SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT &&
	      memcmp(&stream, &before, sizeof(stream)) == 0,
	      "invalid stream call changed state");
	stream.b1_active = true;
	CHECK(!sippymodem_v34_data_stream_is_valid(&stream),
	      "B1 state accepted an ordinary data frame");
	CHECK(strcmp(sippymodem_v34_data_stream_result_name(
		     SIPPYMODEM_V34_DATA_STREAM_OK), "ok") == 0 &&
	      strcmp(sippymodem_v34_data_stream_result_name(
		     SIPPYMODEM_V34_DATA_STREAM_SOURCE_FAILURE),
		     "mapping-frame source failure") == 0 &&
	      strcmp(sippymodem_v34_data_stream_result_name(
		     SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_data_stream_result_name(
		     SIPPYMODEM_V34_DATA_STREAM_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(sippymodem_v34_data_stream_result_name(
		     SIPPYMODEM_V34_DATA_STREAM_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(sippymodem_v34_data_stream_result_name(
		     SIPPYMODEM_V34_DATA_STREAM_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_data_stream_result_name(
		     (enum sippymodem_v34_data_stream_result)99),
		     "unknown") == 0,
	      "data-stream result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
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
