// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapping_frame_transmitter.h>
#include <v34_channel_frame_decoder.h>
#include <v34_data_receiver.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	PIPELINE_MAPPING_FRAMES =
		V34_SURVIVOR_TRACEBACK_INTERVALS /
		V34_4D_INTERVALS_PER_MAPPING_FRAME,
	MAX_GENERATED_MAPPING_FRAMES = 48,
};

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct configuration {
	enum v34_symbol_rate symbol_rate;
	uint8_t primary_rate;
	bool auxiliary;
	bool expanded;
	enum v34_station_role transmitting_role;
};

static const struct configuration CONFIGURATIONS[] = {
	{ V34_SYMBOL_RATE_3000, 8, true, false,
	  V34_CALLING_STATION },
	{ V34_SYMBOL_RATE_3429, 14, false, true,
	  V34_ANSWERING_STATION },
};

struct fixture {
	struct v34_frame_geometry geometry;
	struct v34_frame_schedule transmit_schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_channel_frame_decoder channel_decoder;
	struct v34_data_receiver data_receiver;
	bool transmitting_b1;
};

struct expected_mapping_frame {
	struct v34_mapping_frame mapping_frame;
	struct v34_decided_4d_interval decided[4];
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	bool b1;
};

static bool initialize_fixture(
	struct fixture *fixture, const struct configuration *configuration,
	enum v34_trellis_selection selection)
{
	struct v34_mapping_parameters parameters;

	memset(fixture, 0, sizeof(*fixture));
	fixture->transmitting_b1 = true;
	return v34_frame_geometry_init(
		       &fixture->geometry, configuration->symbol_rate,
		       configuration->primary_rate, configuration->auxiliary) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       &parameters, &fixture->geometry,
		       configuration->expanded) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_b1_frame_schedule_init(
		       &fixture->transmit_schedule, &fixture->geometry) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_transmit_scrambler_init(
		       &fixture->scrambler,
		       configuration->transmitting_role) ==
		       V34_FRAMING_OK &&
	       v34_4d_encoder_init(
		       &fixture->encoder, &parameters, COEFFICIENTS,
		       selection) == V34_4D_ENCODER_OK &&
	       v34_channel_frame_decoder_init(
		       &fixture->channel_decoder, &fixture->geometry,
		       configuration->expanded, COEFFICIENTS, selection) ==
		       V34_CHANNEL_FRAME_DECODER_OK &&
	       v34_data_receiver_init(
		       &fixture->data_receiver, &fixture->geometry,
		       configuration->expanded,
		       configuration->transmitting_role) ==
		       V34_DATA_RECEIVER_OK;
}

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double generated_noise(uint32_t *state)
{
	double unit = (double)(next_generated_word(state) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.5;
}

static void generate_primary_bits(
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS], uint8_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (uint8_t index = 0; index < count; ++index)
		bits[index] = (uint8_t)(next_generated_word(&state) >> 31);
}

static bool prepare_next_mapping_frame(
	struct fixture *fixture, size_t frame_index,
	struct expected_mapping_frame *expected,
	struct v34_transmitted_mapping_frame *transmitted)
{
	struct v34_frame_schedule preview;
	const uint8_t *auxiliary;

	if (v34_frame_schedule_is_complete(
		    &fixture->transmit_schedule)) {
		fixture->transmitting_b1 = false;
		if (v34_frame_schedule_advance_data_frame(
			    &fixture->transmit_schedule) !=
		    V34_FRAME_SCHEDULE_OK)
			return false;
	}
	preview = fixture->transmit_schedule;
	if (v34_frame_schedule_next(
		    &preview, &expected->mapping_frame) !=
	    V34_FRAME_SCHEDULE_OK)
		return false;
	expected->b1 = fixture->transmitting_b1;
	if (expected->b1) {
		memset(expected->primary_bits, 1,
		       expected->mapping_frame.primary_bits);
		expected->auxiliary_bit = 1;
	} else {
		generate_primary_bits(
			expected->primary_bits,
			expected->mapping_frame.primary_bits,
			UINT32_C(0x6d2b79f5) ^ (uint32_t)frame_index);
		expected->auxiliary_bit = (uint8_t)(frame_index & 1u);
	}
	auxiliary = expected->mapping_frame.carries_auxiliary_bit ?
		&expected->auxiliary_bit :
		NULL;
	if (v34_transmit_mapping_frame(
		    &fixture->transmit_schedule, &fixture->scrambler,
		    &fixture->encoder, expected->primary_bits,
		    expected->mapping_frame.primary_bits, auxiliary,
		    V34_NONLINEAR_THETA_ZERO, 512.0,
		    transmitted) !=
	    V34_MAPPING_FRAME_TRANSMITTER_OK)
		return false;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		expected->decided[interval].trellis_output =
			transmitted->encoded.interval[interval].trellis_output;
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol)
			expected->decided[interval].mapper_output[symbol] =
				transmitted->encoded.interval[interval]
					.mapped[symbol]
					.mapper_output;
	}
	return true;
}

static bool survivor_decision_matches(
	const struct v34_survivor_decision *decision,
	const struct v34_4d_encoder_output *encoded,
	uint8_t inversion_bit)
{
	if (decision->convolutional_output !=
		    encoded->convolutional_output ||
	    decision->inversion_bit != inversion_bit)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (memcmp(
			    &decision->channel_output[symbol],
			    &encoded->precoded[symbol].channel_output,
			    sizeof(decision->channel_output[symbol])) != 0)
			return false;
	}
	return true;
}

static bool received_matches(
	const struct v34_received_mapping_frame *received,
	const struct expected_mapping_frame *expected)
{
	const struct v34_mapping_frame *left =
		&received->mapping_frame;
	const struct v34_mapping_frame *right =
		&expected->mapping_frame;

	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame &&
	       received->primary_bit_count == right->primary_bits &&
	       memcmp(received->primary_bits, expected->primary_bits,
		      right->primary_bits) == 0 &&
	       received->auxiliary_bit_count ==
		       (uint8_t)right->carries_auxiliary_bit &&
	       (!right->carries_auxiliary_bit ||
		received->auxiliary_bit == expected->auxiliary_bit);
}

static bool test_generated_b1_and_data_round_trip(void)
{
	for (size_t configuration = 0;
	     configuration < ARRAY_LENGTH(CONFIGURATIONS); ++configuration) {
		for (enum v34_trellis_selection selection =
			     V34_TRELLIS_16_STATE;
		     selection <= V34_TRELLIS_64_STATE;
		     ++selection) {
			struct expected_mapping_frame
				expected[MAX_GENERATED_MAPPING_FRAMES];
			struct fixture fixture;
			uint32_t random = UINT32_C(0x3f74a921) ^
					  ((uint32_t)configuration << 8) ^ selection;
			size_t calls = 0;
			size_t received_frames = 0;
			size_t mapping_frames;

			CHECK(initialize_fixture(
				      &fixture, &CONFIGURATIONS[configuration],
				      selection),
			      "channel frame mode %zu/%u did not initialize",
			      configuration, selection);
			mapping_frames =
				2u * fixture.geometry.mapping_frames_per_data_frame +
				PIPELINE_MAPPING_FRAMES;
			CHECK(mapping_frames <= ARRAY_LENGTH(expected),
			      "channel frame fixture is too small");
			for (size_t frame = 0; frame < mapping_frames; ++frame) {
				struct v34_transmitted_mapping_frame
					transmitted;

				CHECK(prepare_next_mapping_frame(
					      &fixture, frame, &expected[frame],
					      &transmitted),
				      "channel frame %zu/%u/%zu did not transmit",
				      configuration, selection, frame);
				for (uint8_t interval = 0;
				     interval <
					     V34_4D_INTERVALS_PER_MAPPING_FRAME;
				     ++interval) {
					struct sippymodem_qam_point estimates[2];
					struct v34_survivor_decision current;
					struct v34_decided_4d_interval
						decided[4];
					struct v34_decided_4d_interval
						saved_decided[4];
					bool ready = true;
					bool expected_ready;
					uint8_t inversion_bit = (uint8_t)(
						(expected[frame]
							 .mapping_frame.inversion_4d_mask >>
						 interval) &
						1u);

					for (uint8_t symbol = 0; symbol < 2u;
					     ++symbol) {
						estimates[symbol].in_phase =
							transmitted.encoded.interval[interval]
								.precoded[symbol]
								.channel_output.in_phase +
							generated_noise(&random);
						estimates[symbol].quadrature =
							transmitted.encoded.interval[interval]
								.precoded[symbol]
								.channel_output.quadrature +
							generated_noise(&random);
					}
					memset(decided, 0xa5, sizeof(decided));
					memcpy(saved_decided, decided, sizeof(decided));
					calls++;
					expected_ready = calls >= 35u &&
						((calls - 35u) % 4u) == 0u;
					CHECK(v34_channel_frame_decoder_advance(
						      &fixture.channel_decoder, estimates,
						      &current, decided, &ready) ==
						      V34_CHANNEL_FRAME_DECODER_OK &&
					      survivor_decision_matches(
						      &current,
						      &transmitted.encoded.interval[interval],
						      inversion_bit) &&
					      ready == expected_ready,
					      "channel interval %zu/%u/%zu/%u changed",
					      configuration, selection, frame, interval);
					if (!ready) {
						CHECK(memcmp(
							      decided, saved_decided,
							      sizeof(decided)) == 0,
						      "channel frame emitted early");
						continue;
					}
					CHECK(received_frames < frame &&
					      memcmp(decided,
						     expected[received_frames].decided,
						     sizeof(decided)) == 0,
					      "channel frame %zu/%u/%zu decisions changed",
					      configuration, selection, received_frames);
					{
						struct v34_received_mapping_frame
							received;
						enum v34_data_receiver_result
							result;
						enum v34_data_receiver_result
							expected_result;

						result =
							v34_data_receiver_receive_mapping_frame(
								&fixture.data_receiver,
								decided, &received);
						expected_result =
							expected[received_frames].b1 ?
								(expected[received_frames]
									 .mapping_frame
									 .completes_data_frame ?
									 V34_DATA_RECEIVER_B1_COMPLETE :
									 V34_DATA_RECEIVER_B1_IN_PROGRESS) :
								V34_DATA_RECEIVER_OK;
						CHECK(result == expected_result &&
						      received_matches(
							      &received,
							      &expected[received_frames]),
						      "received channel frame %zu/%u/%zu changed",
						      configuration, selection,
						      received_frames);
					}
					received_frames++;
				}
			}
			CHECK(received_frames ==
				      2u * fixture.geometry
						   .mapping_frames_per_data_frame &&
			      !v34_data_receiver_is_receiving_b1(
				      &fixture.data_receiver) &&
			      fixture.data_receiver.b1_error_bits == 0u &&
			      v34_channel_frame_decoder_is_valid(
				      &fixture.channel_decoder),
			      "channel frame mode %zu/%u did not complete",
			      configuration, selection);
		}
	}
	return true;
}

static bool rejected_advance_is_transactional(
	struct v34_channel_frame_decoder *decoder,
	const struct sippymodem_qam_point estimates[2],
	enum v34_channel_frame_decoder_result expected_result)
{
	struct v34_channel_frame_decoder saved = *decoder;
	struct v34_survivor_decision current;
	struct v34_survivor_decision saved_current;
	struct v34_decided_4d_interval decided[4];
	struct v34_decided_4d_interval saved_decided[4];
	bool ready = true;

	memset(&current, 0x5a, sizeof(current));
	memset(decided, 0xa5, sizeof(decided));
	saved_current = current;
	memcpy(saved_decided, decided, sizeof(decided));
	return v34_channel_frame_decoder_advance(
		       decoder, estimates, &current, decided, &ready) ==
		       expected_result &&
	       memcmp(decoder, &saved, sizeof(saved)) == 0 &&
	       memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	       memcmp(decided, saved_decided, sizeof(decided)) == 0 && ready;
}

static bool test_transactional_boundaries(void)
{
	static const struct sippymodem_qam_point estimates[2] = {
		{ 1.0, 1.0 }, { 1.0, 1.0 },
	};
	struct v34_q14_coefficient invalid_coefficients[3] = {
		{ INT16_MIN, 0 }, { 0, 0 }, { 0, 0 },
	};
	struct v34_frame_geometry geometry;
	struct v34_frame_geometry invalid_geometry;
	struct v34_channel_frame_decoder decoder;
	struct v34_channel_frame_decoder saved;
	struct v34_channel_frame_decoder corrupted;
	struct sippymodem_qam_point malformed_estimates[2];
	struct v34_survivor_decision current;
	struct v34_decided_4d_interval decided[4];
	bool ready;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, true) ==
		      V34_FRAME_SCHEDULE_OK,
	      "channel frame boundary geometry did not initialize");
	invalid_geometry = geometry;
	invalid_geometry.mapping_frames_per_data_frame = 0;
	memset(&decoder, 0xa5, sizeof(decoder));
	saved = decoder;
	CHECK(v34_channel_frame_decoder_init(
		      NULL, &geometry, false, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_init(
		      &decoder, NULL, false, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_init(
		      &decoder, &invalid_geometry, false, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_init(
		      &decoder, &geometry, false, NULL,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_init(
		      &decoder, &geometry, false, invalid_coefficients,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_init(
		      &decoder, &geometry, false, COEFFICIENTS,
		      (enum v34_trellis_selection)3) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &saved, sizeof(decoder)) == 0,
	      "invalid channel frame initialization changed its destination");
	CHECK(v34_channel_frame_decoder_init(
		      &decoder, &geometry, false, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_FRAME_DECODER_OK &&
	      v34_channel_frame_decoder_is_valid(&decoder) &&
	      !v34_channel_frame_decoder_is_valid(NULL),
	      "valid channel frame decoder did not initialize");
	saved = decoder;
	memset(&current, 0x5a, sizeof(current));
	memset(decided, 0xa5, sizeof(decided));
	ready = true;
	CHECK(v34_channel_frame_decoder_advance(
		      NULL, estimates, &current, decided, &ready) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_advance(
		      &decoder, NULL, &current, decided, &ready) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_advance(
		      &decoder, estimates, NULL, decided, &ready) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_advance(
		      &decoder, estimates, &current, NULL, &ready) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      v34_channel_frame_decoder_advance(
		      &decoder, estimates, &current, decided, NULL) ==
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &saved, sizeof(saved)) == 0,
	      "null channel frame arguments changed state");
	malformed_estimates[0] = estimates[0];
	malformed_estimates[1] = estimates[1];
	malformed_estimates[0].in_phase = NAN;
	CHECK(rejected_advance_is_transactional(
		      &decoder, malformed_estimates,
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT),
	      "non-finite channel frame estimate was accepted");
	malformed_estimates[0].in_phase = DBL_MAX;
	CHECK(rejected_advance_is_transactional(
		      &decoder, malformed_estimates,
		      V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE),
	      "overflowing channel frame metric was accepted");
	corrupted = decoder;
	corrupted.next_channel_interval = 4;
	CHECK(!v34_channel_frame_decoder_is_valid(&corrupted) &&
	      rejected_advance_is_transactional(
		      &corrupted, estimates,
		      V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT),
	      "corrupt channel interval was accepted");
	corrupted = decoder;
	corrupted.channel_frame.inversion_4d_mask ^= 1u;
	CHECK(!v34_channel_frame_decoder_is_valid(&corrupted),
	      "mismatched channel frame schedule was accepted");
	corrupted = decoder;
	corrupted.pending_interval_count = 1;
	CHECK(!v34_channel_frame_decoder_is_valid(&corrupted),
	      "misaligned pending frame was accepted");
	corrupted = decoder;
	corrupted.channel_decoder.survivor.path_index = 1;
	CHECK(!v34_channel_frame_decoder_is_valid(&corrupted),
	      "misaligned survivor cursor was accepted");
	CHECK(strcmp(v34_channel_frame_decoder_result_name(
		     V34_CHANNEL_FRAME_DECODER_OK),
		     "ok") == 0 &&
	      strcmp(v34_channel_frame_decoder_result_name(
		     V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_channel_frame_decoder_result_name(
		     V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_channel_frame_decoder_result_name(
		     V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v34_channel_frame_decoder_result_name(
		     (enum v34_channel_frame_decoder_result)99),
		     "unknown") == 0,
	      "channel frame result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated B1 and data round trip",
		  test_generated_b1_and_data_round_trip },
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
