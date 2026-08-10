// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_data_transmitter.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct transmitter_fixture {
	struct sippymodem_v34_phase3_profile profile;
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_power_normalizer normalizer;
};

static bool initialize_fixture(struct transmitter_fixture *fixture)
{
	struct transmitter_fixture generated = { 0 };

	generated.profile = (struct sippymodem_v34_phase3_profile){
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
	if (!sippymodem_v34_phase3_profile_is_valid(&generated.profile) ||
	    sippymodem_v34_frame_geometry_init(
		    &generated.geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8,
		    true) != SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry,
		    (uint8_t)(generated.geometry.data_frames_per_superframe - 1u)) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_transmit_scrambler_init(
		    &generated.scrambler, SIPPYMODEM_V34_CALLING_STATION) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    SIPPYMODEM_V34_TRELLIS_32_STATE) !=
		    SIPPYMODEM_V34_4D_ENCODER_OK ||
	    sippymodem_v34_calibrate_mapping_power(
		    &generated.schedule, &generated.scrambler,
		    &generated.encoder,
		    SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
		    &generated.normalizer) !=
		    SIPPYMODEM_V34_POWER_CALIBRATION_OK)
		return false;
	*fixture = generated;
	return true;
}

static void generate_primary_bits(
	uint8_t bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS], size_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (size_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static bool mapping_frames_are_equal(
	const struct sippymodem_v34_mapping_frame *left,
	const struct sippymodem_v34_mapping_frame *right)
{
	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame;
}

static bool prepare_model_schedule(
	struct sippymodem_v34_frame_schedule *schedule)
{
	return !sippymodem_v34_frame_schedule_is_complete(schedule) ||
	       sippymodem_v34_frame_schedule_advance_data_frame(schedule) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

static bool test_streaming_composition(void)
{
	struct transmitter_fixture fixture;
	struct sippymodem_v34_data_transmitter transmitter;
	struct sippymodem_v34_data_transmitter waiting;
	struct sippymodem_v34_frame_schedule model_schedule;
	struct sippymodem_v34_scrambler model_scrambler;
	struct sippymodem_v34_4d_encoder model_encoder;
	struct sippymodem_v34_data_line_transmitter model_line;
	size_t mapping_frames;
	size_t samples = 0;
	bool nonzero_seen = false;
	int16_t waiting_sample = 1234;
	bool waiting_accepted = true;

	CHECK(initialize_fixture(&fixture) &&
	      sippymodem_v34_data_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 5000) == SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
	      sippymodem_v34_data_transmitter_needs_mapping_frame(
		      &transmitter),
	      "streaming transmitter fixture did not initialize");
	model_schedule = fixture.schedule;
	model_scrambler = fixture.scrambler;
	model_encoder = fixture.encoder;
	model_line = transmitter.line;
	waiting = transmitter;
	CHECK(sippymodem_v34_data_transmitter_sample(
		      &transmitter, &waiting_sample, &waiting_accepted) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED &&
	      memcmp(&transmitter, &waiting, sizeof(transmitter)) == 0 &&
	      waiting_sample == 1234 && waiting_accepted,
	      "empty streaming transmitter changed state or outputs");

	mapping_frames = (size_t)fixture.geometry.mapping_frames_per_data_frame +
		1u;
	for (size_t frame_index = 0; frame_index < mapping_frames;
	     ++frame_index) {
		struct sippymodem_v34_frame_schedule preview_schedule;
		struct sippymodem_v34_mapping_frame expected_request;
		struct sippymodem_v34_mapping_frame actual_request;
		struct sippymodem_v34_transmitted_mapping_frame model_frame;
		uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
		uint8_t auxiliary_bit = (uint8_t)(frame_index & 1u);
		const uint8_t *auxiliary_pointer;

		CHECK(prepare_model_schedule(&model_schedule),
		      "model schedule did not advance at frame %zu", frame_index);
		preview_schedule = model_schedule;
		CHECK(sippymodem_v34_frame_schedule_next(
			      &preview_schedule, &expected_request) ==
			      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
		      sippymodem_v34_data_transmitter_preview_mapping_frame(
			      &transmitter, &actual_request) ==
			      SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
		      mapping_frames_are_equal(
			      &actual_request, &expected_request),
		      "mapping request diverged at frame %zu", frame_index);
		generate_primary_bits(
			primary_bits, expected_request.primary_bits,
			UINT32_C(0x71073ebd) ^ (uint32_t)frame_index);
		auxiliary_pointer = expected_request.carries_auxiliary_bit ?
			&auxiliary_bit : NULL;
		CHECK(sippymodem_v34_transmit_mapping_frame(
			      &model_schedule, &model_scrambler, &model_encoder,
			      primary_bits, expected_request.primary_bits,
			      auxiliary_pointer,
			      fixture.normalizer.nonlinear_parameter,
			      fixture.normalizer.precoded_mean_squared_magnitude,
			      &model_frame) ==
			      SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK &&
		      sippymodem_v34_data_line_transmitter_load_mapping_frame(
			      &model_line, &model_frame) ==
			      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
		      sippymodem_v34_data_transmitter_queue_mapping_frame(
			      &transmitter, primary_bits,
			      expected_request.primary_bits, auxiliary_pointer) ==
			      SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
		      memcmp(&transmitter.schedule, &model_schedule,
			     sizeof(model_schedule)) == 0 &&
		      memcmp(&transmitter.scrambler, &model_scrambler,
			     sizeof(model_scrambler)) == 0 &&
		      memcmp(&transmitter.mapping_encoder, &model_encoder,
			     sizeof(model_encoder)) == 0 &&
		      memcmp(&transmitter.line, &model_line,
			     sizeof(model_line)) == 0,
		      "mapping composition diverged at frame %zu", frame_index);
		if (frame_index == 0) {
			struct sippymodem_v34_mapping_frame pending = {
				.index = UINT8_MAX,
			};
			struct sippymodem_v34_mapping_frame saved = pending;
			struct sippymodem_v34_data_transmitter queued = transmitter;

			CHECK(sippymodem_v34_data_transmitter_preview_mapping_frame(
				      &transmitter, &pending) ==
				      SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING &&
			      mapping_frames_are_equal(&pending, &saved) &&
			      sippymodem_v34_data_transmitter_queue_mapping_frame(
				      &transmitter, primary_bits,
				      expected_request.primary_bits,
				      auxiliary_pointer) ==
				      SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING &&
			      memcmp(&transmitter, &queued, sizeof(transmitter)) == 0,
			      "pending frame was replaced");
		}
		while (!sippymodem_v34_data_line_transmitter_needs_mapping_frame(
			&model_line)) {
			int16_t expected_sample;
			int16_t actual_sample;
			bool expected_accepted;
			bool actual_accepted;

			CHECK(samples < 1024u &&
			      !sippymodem_v34_data_transmitter_needs_mapping_frame(
				      &transmitter) &&
			      sippymodem_v34_data_line_transmitter_sample(
				      &model_line, &expected_sample,
				      &expected_accepted) ==
				      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
			      sippymodem_v34_data_transmitter_sample(
				      &transmitter, &actual_sample,
				      &actual_accepted) ==
				      SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
			      actual_sample == expected_sample &&
			      actual_accepted == expected_accepted &&
			      memcmp(&transmitter.line, &model_line,
				     sizeof(model_line)) == 0,
			      "line output diverged after sample %zu", samples);
			nonzero_seen |= actual_sample != 0;
			samples++;
		}
		CHECK(sippymodem_v34_data_transmitter_needs_mapping_frame(
			      &transmitter),
		      "frame %zu did not return backpressure", frame_index);
	}
	CHECK(model_schedule.data_frame_in_superframe == 0 &&
	      model_schedule.next_mapping_frame == 1 &&
	      transmitter.schedule.data_frame_in_superframe == 0 &&
	      transmitter.schedule.next_mapping_frame == 1 &&
	      samples > mapping_frames * SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME &&
	      nonzero_seen,
	      "stream did not cross the superframe boundary");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "streaming composition", test_streaming_composition },
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
