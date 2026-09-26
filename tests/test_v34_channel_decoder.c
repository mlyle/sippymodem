// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_encoder.h>
#include <v34_channel_decoder.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	GENERATED_INTERVALS = 128,
};

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct channel_profile {
	enum v34_symbol_rate symbol_rate;
	uint8_t primary_rate;
	bool expanded;
};

static const struct channel_profile PROFILES[] = {
	{ V34_SYMBOL_RATE_3000, 8, false },
	{ V34_SYMBOL_RATE_2743, 8, false },
	{ V34_SYMBOL_RATE_3429, 14, true },
};

struct expected_interval {
	struct v34_4d_encoder_input input;
	struct v34_grid_point mapper_output[2];
	uint8_t trellis_output;
	uint8_t differential_output;
};

static bool initialize_parameters(
	const struct channel_profile *profile,
	struct v34_frame_geometry *geometry,
	struct v34_mapping_parameters *parameters)
{
	return v34_frame_geometry_init(
		       geometry, profile->symbol_rate, profile->primary_rate,
		       false) == V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       parameters, geometry, profile->expanded) ==
		       V34_MAPPING_PARAMETERS_OK;
}

static struct v34_4d_encoder_input generated_input(
	const struct v34_mapping_parameters *parameters,
	enum v34_trellis_selection selection, size_t interval)
{
	uint8_t point_count = (uint8_t)(
		1u << parameters->point_index_bits_per_2d_symbol);

	return (struct v34_4d_encoder_input){
		.mapping = {
			.rotation_bit = (uint8_t)((interval >> 1) & 1u),
			.differential_input =
				(uint8_t)((3u * interval + selection) & 3u),
			.point_index = {
				(uint8_t)((5u * interval + selection) %
					  point_count),
				(uint8_t)((7u * interval + 1u) % point_count),
			},
		},
		.ring_index = {
			(uint8_t)((interval + selection) % parameters->ring_count),
			(uint8_t)((3u * interval + 2u) %
				  parameters->ring_count),
		},
		.inversion_bit = (uint8_t)((interval / 5u) & 1u),
	};
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

	return (unit - 0.5) * 0.75;
}

static struct v34_survivor_decision encoded_decision(
	const struct v34_4d_encoder_output *encoded,
	uint8_t inversion_bit)
{
	struct v34_survivor_decision decision = {
		.convolutional_output = encoded->convolutional_output,
		.inversion_bit = inversion_bit,
	};

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		decision.channel_output[symbol] =
			encoded->precoded[symbol].channel_output;
	return decision;
}

static bool decisions_are_equal(
	const struct v34_survivor_decision *left,
	const struct v34_survivor_decision *right)
{
	return left->convolutional_output == right->convolutional_output &&
	       left->inversion_bit == right->inversion_bit &&
	       memcmp(
		       left->channel_output, right->channel_output,
		       sizeof(left->channel_output)) == 0;
}

static bool decided_matches(
	const struct v34_decided_4d_interval *decided,
	const struct expected_interval *expected)
{
	return decided->trellis_output == expected->trellis_output &&
	       memcmp(
		       decided->mapper_output, expected->mapper_output,
		       sizeof(decided->mapper_output)) == 0;
}

static bool decoded_matches(
	const struct v34_decoded_4d_interval *decoded,
	const struct expected_interval *expected)
{
	return decoded->mapping.rotation_bit ==
		       expected->input.mapping.rotation_bit &&
	       decoded->mapping.differential_input ==
		       expected->input.mapping.differential_input &&
	       memcmp(
		       decoded->mapping.point_index,
		       expected->input.mapping.point_index,
		       sizeof(decoded->mapping.point_index)) == 0 &&
	       decoded->ring_prefix[0] == expected->input.ring_index[0] &&
	       decoded->ring_prefix[1] == expected->input.ring_index[1] &&
	       decoded->differential_output == expected->differential_output;
}

static bool rejected_advance_is_transactional(
	struct v34_channel_decoder *decoder,
	const struct qam_point estimates[2], uint8_t inversion_bit,
	enum v34_channel_decoder_result expected);

static bool test_generated_round_trip(void)
{
	for (size_t profile_index = 0; profile_index < ARRAY_LENGTH(PROFILES);
	     ++profile_index) {
		struct v34_frame_geometry geometry;
		struct v34_mapping_parameters parameters;

		CHECK(initialize_parameters(
			      &PROFILES[profile_index], &geometry, &parameters),
		      "channel profile %zu did not initialize", profile_index);
		for (enum v34_trellis_selection selection =
			     V34_TRELLIS_16_STATE;
		     selection <= V34_TRELLIS_64_STATE;
		     ++selection) {
			struct expected_interval expected[GENERATED_INTERVALS];
			struct v34_4d_encoder encoder;
			struct v34_channel_decoder channel_decoder;
			struct v34_4d_decoder mapping_decoder;
			uint32_t random = UINT32_C(0x3f74a921) + selection;

			CHECK(v34_4d_encoder_init(
				      &encoder, &parameters, COEFFICIENTS,
				      selection) == V34_4D_ENCODER_OK &&
			      v34_channel_decoder_init(
				      &channel_decoder, &geometry,
				      PROFILES[profile_index].expanded,
				      COEFFICIENTS, selection) ==
				      V34_CHANNEL_DECODER_OK &&
			      v34_4d_decoder_init(
				      &mapping_decoder, &parameters) ==
				      V34_4D_DECODER_OK,
			      "channel mode %zu/%u did not initialize",
			      profile_index, selection);
			for (size_t interval = 0;
			     interval < GENERATED_INTERVALS; ++interval) {
				struct v34_4d_encoder_output encoded;
				struct v34_survivor_decision current;
				struct v34_survivor_decision expected_current;
				struct v34_decided_4d_interval decided;
				struct v34_decided_4d_interval saved_decided;
				struct v34_decoded_4d_interval decoded;
				struct qam_point estimates[2];
				bool ready = true;

				expected[interval].input = generated_input(
					&parameters, selection, interval);
				CHECK(v34_4d_encoder_step(
					      &encoder, &expected[interval].input,
					      &encoded) ==
					      V34_4D_ENCODER_OK,
				      "channel encode %zu/%u/%zu failed",
				      profile_index, selection, interval);
				expected_current = encoded_decision(
					&encoded, expected[interval].input.inversion_bit);
				for (uint8_t symbol = 0;
				     symbol <
					     V34_2D_SYMBOLS_PER_4D_INTERVAL;
				     ++symbol) {
					expected[interval].mapper_output[symbol] =
						encoded.mapped[symbol].mapper_output;
					estimates[symbol] =
						(struct qam_point){
							.in_phase = encoded.precoded[symbol]
									    .channel_output.in_phase +
								    generated_noise(&random),
							.quadrature = encoded.precoded[symbol]
									      .channel_output.quadrature +
								      generated_noise(&random),
						};
				}
				expected[interval].trellis_output =
					encoded.trellis_output;
				expected[interval].differential_output =
					encoded.differential_output;
				if (interval + 1u >= V34_SURVIVOR_TRACEBACK_INTERVALS) {
					struct v34_precoder saved_precoder =
						channel_decoder.inverse_precoder;

					channel_decoder.inverse_precoder =
						(struct v34_precoder){
							.coefficients = {
								{ INT16_MAX, 0 },
								{ INT16_MAX, 0 }, { 0, 0 },
							},
							.previous_symbols = {
								{ 1000000000, 0 },
								{ -1000000000, 0 }, { 0, 0 },
							},
							.grid_step = saved_precoder.grid_step,
						};
					/* Retry the same late rejection at every ring position,
					 * including first traceback and multiple wraps. */
					for (unsigned retry = 0; retry < 2; ++retry)
						CHECK(v34_channel_decoder_is_valid(&channel_decoder) &&
						      rejected_advance_is_transactional(
							      &channel_decoder, estimates,
							      expected[interval].input.inversion_bit,
							      V34_CHANNEL_DECODER_OUT_OF_RANGE),
						      "late rollback %zu/%u/%zu/%u changed state",
						      profile_index, selection, interval, retry);
					channel_decoder.inverse_precoder = saved_precoder;
				}
				memset(&decided, 0xa5, sizeof(decided));
				saved_decided = decided;
				CHECK(v34_channel_decoder_advance(
					      &channel_decoder, estimates,
					      expected[interval].input.inversion_bit,
					      &current, &decided, &ready) ==
					      V34_CHANNEL_DECODER_OK &&
				      v34_channel_decoder_is_valid(&channel_decoder) &&
				      decisions_are_equal(
					      &current, &expected_current) &&
				      ready ==
					      (interval + 1u >=
					       V34_SURVIVOR_TRACEBACK_INTERVALS),
				      "channel decision %zu/%u/%zu changed",
				      profile_index, selection, interval);
				if (ready) {
					size_t recovered =
						interval + 1u -
						V34_SURVIVOR_TRACEBACK_INTERVALS;

					CHECK(decided_matches(
						      &decided, &expected[recovered]) &&
					      v34_decode_4d_interval(
						      &mapping_decoder, &decided,
						      &decoded) ==
						      V34_4D_DECODER_OK &&
					      decoded_matches(
						      &decoded, &expected[recovered]),
					      "channel traceback %zu/%u/%zu changed",
					      profile_index, selection, recovered);
				} else {
					CHECK(memcmp(
						      &decided, &saved_decided,
						      sizeof(decided)) == 0,
					      "channel emitted before traceback");
				}
			}
			CHECK(v34_channel_decoder_is_valid(
				      &channel_decoder),
			      "channel mode %zu/%u ended invalid", profile_index,
			      selection);
		}
	}
	return true;
}

static bool rejected_advance_is_transactional(
	struct v34_channel_decoder *decoder,
	const struct qam_point estimates[2], uint8_t inversion_bit,
	enum v34_channel_decoder_result expected)
{
	struct v34_channel_decoder saved = *decoder;
	struct v34_survivor_decision current;
	struct v34_survivor_decision saved_current;
	struct v34_decided_4d_interval decided;
	struct v34_decided_4d_interval saved_decided;
	bool ready = true;

	memset(&current, 0x5a, sizeof(current));
	memset(&decided, 0xa5, sizeof(decided));
	saved_current = current;
	saved_decided = decided;
	return v34_channel_decoder_advance(
		       decoder, estimates, inversion_bit, &current, &decided,
		       &ready) == expected &&
	       memcmp(decoder, &saved, sizeof(saved)) == 0 &&
	       memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	       memcmp(&decided, &saved_decided, sizeof(decided)) == 0 && ready;
}

static bool test_transactional_boundaries(void)
{
	static const struct qam_point estimates[2] = {
		{ 1.0, 1.0 }, { 1.0, 1.0 },
	};
	static const struct v34_q14_coefficient zero_coefficients
		[V34_PRECODER_TAP_COUNT] = { 0 };
	struct v34_frame_geometry geometry;
	struct v34_frame_geometry malformed_geometry;
	struct v34_mapping_parameters parameters;
	struct v34_channel_decoder decoder;
	struct v34_channel_decoder saved;
	struct v34_channel_decoder corrupted;
	struct qam_point malformed_estimates[2];
	struct v34_survivor_decision current;
	struct v34_decided_4d_interval decided;
	bool ready;

	memset(&decoder, 0xa5, sizeof(decoder));
	saved = decoder;
	CHECK(initialize_parameters(&PROFILES[0], &geometry, &parameters),
	      "channel boundary parameters did not initialize");
	malformed_geometry = geometry;
	malformed_geometry.mapping_frames_per_data_frame++;
	CHECK(v34_channel_decoder_init(
		      NULL, &geometry, false, zero_coefficients,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
	      v34_channel_decoder_init(
		      &decoder, NULL, false, zero_coefficients,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
	      v34_channel_decoder_init(
		      &decoder, &geometry, false, NULL,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
	      v34_channel_decoder_init(
		      &decoder, &malformed_geometry, false, zero_coefficients,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
	      v34_channel_decoder_init(
		      &decoder, &geometry, false, zero_coefficients,
		      (enum v34_trellis_selection)3) ==
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
	      v34_channel_decoder_init(
		      &decoder,
		      (const struct v34_frame_geometry *)&decoder, false,
		      zero_coefficients, V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &saved, sizeof(decoder)) == 0,
	      "invalid channel initialization changed its destination");
	CHECK(v34_channel_decoder_init(
		      &decoder, &geometry, false, zero_coefficients,
		      V34_TRELLIS_16_STATE) ==
		      V34_CHANNEL_DECODER_OK &&
	      v34_channel_decoder_is_valid(&decoder) &&
	      !v34_channel_decoder_is_valid(NULL),
	      "valid channel decoder did not initialize");
	{
		struct v34_channel_decoder before = decoder;
		struct v34_survivor_decision saved_current;
		struct v34_decided_4d_interval saved_decided;

		memset(&current, 0x5a, sizeof(current));
		memset(&decided, 0xa5, sizeof(decided));
		saved_current = current;
		saved_decided = decided;
		ready = true;
		CHECK(v34_channel_decoder_advance(
			      NULL, estimates, 0, &current, &decided, &ready) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      v34_channel_decoder_advance(
			      &decoder, estimates, 0, NULL, &decided, &ready) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      v34_channel_decoder_advance(
			      &decoder, estimates, 0, &current, NULL, &ready) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      v34_channel_decoder_advance(
			      &decoder, estimates, 0, &current, &decided, NULL) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      memcmp(&decoder, &before, sizeof(before)) == 0 &&
		      memcmp(&current, &saved_current, sizeof(current)) == 0 &&
		      memcmp(&decided, &saved_decided, sizeof(decided)) == 0 &&
		      ready,
		      "null channel outputs changed state");
	}
	CHECK(rejected_advance_is_transactional(
		      &decoder, NULL, 0,
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT) &&
	      rejected_advance_is_transactional(
		      &decoder, estimates, 2,
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT),
	      "invalid channel input was not transactional");
	{
		struct v34_channel_decoder before = decoder;
		struct v34_survivor_decision aliased;
		struct v34_survivor_decision saved_aliased;
		struct v34_decided_4d_interval output;
		struct v34_decided_4d_interval saved_output;
		bool output_ready = true;

		memset(&aliased, 0x5a, sizeof(aliased));
		saved_aliased = aliased;
		memset(&output, 0xa5, sizeof(output));
		saved_output = output;
		CHECK(v34_channel_decoder_advance(
			      &decoder, (const struct qam_point *)&decoder, 0,
			      &aliased, &output, &output_ready) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      v34_channel_decoder_advance(
			      &decoder, estimates, 0,
			      (struct v34_survivor_decision *)&decoder, &output,
			      &output_ready) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      v34_channel_decoder_advance(
			      &decoder, estimates, 0, &aliased,
			      (struct v34_decided_4d_interval *)&aliased,
			      &output_ready) ==
			      V34_CHANNEL_DECODER_INVALID_ARGUMENT &&
		      memcmp(&decoder, &before, sizeof(before)) == 0 &&
		      memcmp(&aliased, &saved_aliased, sizeof(aliased)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
		      output_ready,
		      "overlapping channel arguments changed state or outputs");
	}
	malformed_estimates[0] = estimates[0];
	malformed_estimates[1] = estimates[1];
	malformed_estimates[0].in_phase = NAN;
	CHECK(rejected_advance_is_transactional(
		      &decoder, malformed_estimates, 0,
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT),
	      "non-finite channel estimate was accepted");
	malformed_estimates[0].in_phase = DBL_MAX;
	CHECK(rejected_advance_is_transactional(
		      &decoder, malformed_estimates, 0,
		      V34_CHANNEL_DECODER_NUMERIC_FAILURE),
	      "overflowing channel metric was accepted");
	corrupted = decoder;
	corrupted.survivor.selection = (enum v34_trellis_selection)3;
	CHECK(!v34_channel_decoder_is_valid(&corrupted) &&
	      rejected_advance_is_transactional(
		      &corrupted, estimates, 0,
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT),
	      "corrupt channel state was accepted");
	corrupted = decoder;
	corrupted.survivor.path_index = 1;
	CHECK(!v34_channel_decoder_is_valid(&corrupted) &&
	      rejected_advance_is_transactional(
		      &corrupted, estimates, 0,
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT),
	      "corrupt survivor state was accepted");
	corrupted = decoder;
	corrupted.inverse_precoder.grid_step =
		V34_PRECODER_GRID_STEP_4;
	CHECK(!v34_channel_decoder_is_valid(&corrupted) &&
	      rejected_advance_is_transactional(
		      &corrupted, estimates, 0,
		      V34_CHANNEL_DECODER_INVALID_ARGUMENT),
	      "mismatched channel grid was accepted");

	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated round trip", test_generated_round_trip },
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
