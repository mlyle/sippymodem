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

static bool rejected_queue_is_transactional(
	struct sippymodem_v34_data_transmitter *transmitter,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit)
{
	struct sippymodem_v34_data_transmitter before = *transmitter;

	return sippymodem_v34_data_transmitter_queue_mapping_frame(
		       transmitter, primary_bits, primary_bit_count,
		       auxiliary_bit) ==
		       SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	       memcmp(transmitter, &before, sizeof(before)) == 0;
}

static bool test_transactional_validation(void)
{
	struct transmitter_fixture fixture;
	struct sippymodem_v34_data_transmitter transmitter;
	struct sippymodem_v34_data_transmitter before;
	struct sippymodem_v34_data_transmitter corrupt;
	struct sippymodem_v34_frame_geometry other_geometry;
	struct sippymodem_v34_frame_schedule other_schedule;
	struct sippymodem_v34_mapping_parameters expanded_parameters;
	struct sippymodem_v34_4d_encoder expanded_encoder;
	struct sippymodem_v34_scrambler other_scrambler;
	struct sippymodem_v34_mapping_frame request = { 0 };
	struct sippymodem_v34_mapping_frame saved_request;
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t malformed_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit = 1;
	uint8_t invalid_auxiliary = 2;
	const uint8_t *auxiliary_pointer;
	const uint8_t *wrong_auxiliary_pointer;
	int16_t sample = 1234;
	bool accepted = true;

	CHECK(initialize_fixture(&fixture),
	      "validation fixture did not initialize");
	memset(&transmitter, 0xa5, sizeof(transmitter));
	CHECK(sippymodem_v34_data_transmitter_init(
		      NULL, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 5000) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_init(
		      &transmitter, NULL, &fixture.scrambler, &fixture.encoder,
		      &fixture.normalizer, &fixture.profile, 8000, 5000) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 0) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, INT16_MAX) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_CLIPPING_RISK &&
	      bytes_are_filled(&transmitter, sizeof(transmitter), 0xa5),
	      "rejected streaming initialization changed its destination");
	CHECK(sippymodem_v34_data_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 5000) == SIPPYMODEM_V34_DATA_TRANSMITTER_OK,
	      "valid streaming state did not initialize");
	before = transmitter;
	saved_request = request;
	CHECK(!sippymodem_v34_data_transmitter_is_valid(NULL) &&
	      !sippymodem_v34_data_transmitter_needs_mapping_frame(NULL) &&
	      sippymodem_v34_data_transmitter_preview_mapping_frame(
		      NULL, &request) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_preview_mapping_frame(
		      &transmitter, NULL) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_queue_mapping_frame(
		      NULL, NULL, 0, NULL) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_sample(
		      NULL, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_sample(
		      &transmitter, NULL, &accepted) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_transmitter_sample(
		      &transmitter, &sample, NULL) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      mapping_frames_are_equal(&request, &saved_request) &&
	      sample == 1234 && accepted,
	      "invalid streaming operation changed state or output");

	transmitter.schedule.next_mapping_frame =
		transmitter.schedule.geometry.mapping_frames_per_data_frame;
	CHECK(sippymodem_v34_data_transmitter_is_valid(&transmitter),
	      "completed schedule was not a valid streaming boundary");
	before = transmitter;
	CHECK(sippymodem_v34_data_transmitter_preview_mapping_frame(
		      &transmitter, &request) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      request.index == 0,
	      "completed-schedule preview changed state");
	generate_primary_bits(
		primary_bits, request.primary_bits, UINT32_C(0x483e0765));
	memcpy(malformed_bits, primary_bits, request.primary_bits);
	malformed_bits[request.primary_bits / 2u] = 2;
	auxiliary_pointer = request.carries_auxiliary_bit ?
		&auxiliary_bit : NULL;
	wrong_auxiliary_pointer = request.carries_auxiliary_bit ?
		&invalid_auxiliary : &auxiliary_bit;
	CHECK(rejected_queue_is_transactional(
		      &transmitter, primary_bits,
		      (size_t)request.primary_bits - 1u, auxiliary_pointer) &&
	      rejected_queue_is_transactional(
		      &transmitter, malformed_bits, request.primary_bits,
		      auxiliary_pointer) &&
	      rejected_queue_is_transactional(
		      &transmitter, primary_bits, request.primary_bits,
		      wrong_auxiliary_pointer),
	      "invalid frame input consumed schedule rollover");
	CHECK(sippymodem_v34_data_transmitter_queue_mapping_frame(
		      &transmitter, primary_bits, request.primary_bits,
		      auxiliary_pointer) == SIPPYMODEM_V34_DATA_TRANSMITTER_OK &&
	      transmitter.schedule.data_frame_in_superframe == 0 &&
	      transmitter.schedule.next_mapping_frame == 1,
	      "valid frame did not commit superframe rollover");

	CHECK(sippymodem_v34_frame_geometry_init(
		      &other_geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 7,
		      true) == SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_frame_schedule_init(
		      &other_schedule, &other_geometry, 0) ==
		      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &expanded_parameters, &fixture.geometry, true) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	      sippymodem_v34_4d_encoder_init(
		      &expanded_encoder, &expanded_parameters, COEFFICIENTS,
		      SIPPYMODEM_V34_TRELLIS_32_STATE) ==
		      SIPPYMODEM_V34_4D_ENCODER_OK &&
	      sippymodem_v34_transmit_scrambler_init(
		      &other_scrambler, SIPPYMODEM_V34_ANSWERING_STATION) ==
		      SIPPYMODEM_V34_FRAMING_OK,
	      "mismatched valid owners did not initialize");
	corrupt = transmitter;
	corrupt.schedule = other_schedule;
	CHECK(!sippymodem_v34_data_transmitter_is_valid(&corrupt),
	      "stream accepted a different frame geometry");
	corrupt = transmitter;
	corrupt.mapping_encoder = expanded_encoder;
	CHECK(!sippymodem_v34_data_transmitter_is_valid(&corrupt),
	      "stream accepted different mapping parameters");
	corrupt = transmitter;
	corrupt.scrambler = other_scrambler;
	before = corrupt;
	CHECK(!sippymodem_v34_data_transmitter_is_valid(&corrupt) &&
	      sippymodem_v34_data_transmitter_sample(
		      &corrupt, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&corrupt, &before, sizeof(corrupt)) == 0 &&
	      sample == 1234 && accepted,
	      "stream accepted the other role's scrambler");
	CHECK(strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED),
		     "mapping frame required") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING),
		     "mapping frame pending") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_data_transmitter_result_name(
		     (enum sippymodem_v34_data_transmitter_result)99),
		     "unknown") == 0,
	      "streaming result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "streaming composition", test_streaming_composition },
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
