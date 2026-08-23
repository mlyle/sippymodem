// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_encoder.h>
#include <v34_path_decoder.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	GENERATED_INTERVALS = 192,
};

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct path_profile {
	enum v34_symbol_rate symbol_rate;
	uint8_t primary_rate;
	enum v34_precoder_grid_step grid_step;
};

static const struct path_profile PROFILES[] = {
	{ V34_SYMBOL_RATE_3000, 8,
	  V34_PRECODER_GRID_STEP_2 },
	{ V34_SYMBOL_RATE_2743, 8,
	  V34_PRECODER_GRID_STEP_4 },
};

static bool initialize_parameters(
	const struct path_profile *profile,
	struct v34_mapping_parameters *parameters)
{
	struct v34_frame_geometry geometry;

	return v34_frame_geometry_init(
		       &geometry, profile->symbol_rate, profile->primary_rate,
		       false) == V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       parameters, &geometry, false) ==
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

static struct v34_survivor_decision survivor_decision(
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

static bool grid_points_are_equal(
	const struct v34_grid_point *left,
	const struct v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool precoders_are_equal(
	const struct v34_precoder *left,
	const struct v34_precoder *right)
{
	if (left->grid_step != right->grid_step)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (left->coefficients[tap].real !=
			    right->coefficients[tap].real ||
		    left->coefficients[tap].imaginary !=
			    right->coefficients[tap].imaginary ||
		    left->previous_symbols[tap].in_phase !=
			    right->previous_symbols[tap].in_phase ||
		    left->previous_symbols[tap].quadrature !=
			    right->previous_symbols[tap].quadrature)
			return false;
	}
	return true;
}

static bool decoded_matches(
	const struct v34_decoded_4d_interval *decoded,
	const struct v34_4d_encoder_input *input,
	uint8_t differential_output)
{
	return decoded->mapping.rotation_bit == input->mapping.rotation_bit &&
	       decoded->mapping.differential_input ==
		       input->mapping.differential_input &&
	       memcmp(
		       decoded->mapping.point_index,
		       input->mapping.point_index,
		       sizeof(decoded->mapping.point_index)) == 0 &&
	       memcmp(
		       decoded->ring_index, input->ring_index,
		       sizeof(decoded->ring_index)) == 0 &&
	       decoded->differential_output == differential_output;
}

static bool test_known_path(void)
{
	struct v34_precoder precoder = {
		.coefficients = { { 8192, 0 } },
		.previous_symbols = { { 512, -256 } },
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	const struct v34_survivor_decision survivor = {
		.channel_output = { { 1, 1 }, { 1, 1 } },
	};
	const struct v34_grid_point expected_mapper[] = {
		{ -1, 1 }, { 1, 1 },
	};
	struct v34_decided_4d_interval decided;

	/* The first inverse offset is (2, 0), while the second is (0, 0).
	 * Their unequal modulo parities make C0 one; Y0 and V0 are zero. */
	CHECK(v34_decode_survivor_path(
		      &precoder, &survivor, &decided) ==
		      V34_PATH_DECODER_OK &&
	      grid_points_are_equal(
		      &decided.mapper_output[0], &expected_mapper[0]) &&
	      grid_points_are_equal(
		      &decided.mapper_output[1], &expected_mapper[1]) &&
	      decided.trellis_output == 1,
	      "known survivor path did not recover C0 or U0");
	return true;
}

static bool test_generated_round_trip(void)
{
	bool modulo_seen[2] = { false };
	bool convolutional_seen[2] = { false };
	bool inversion_seen[2] = { false };
	bool trellis_seen[2] = { false };
	bool nonzero_offset_seen[2] = { false };

	for (size_t profile_index = 0; profile_index < ARRAY_LENGTH(PROFILES);
	     ++profile_index) {
		struct v34_mapping_parameters parameters;

		CHECK(initialize_parameters(
			      &PROFILES[profile_index], &parameters),
		      "path profile %zu did not initialize", profile_index);
		for (enum v34_trellis_selection selection =
			     V34_TRELLIS_16_STATE;
		     selection <= V34_TRELLIS_64_STATE;
		     ++selection) {
			struct v34_4d_encoder encoder;
			struct v34_4d_decoder mapping_decoder;
			struct v34_precoder inverse_precoder;

			CHECK(v34_4d_encoder_init(
				      &encoder, &parameters, COEFFICIENTS,
				      selection) == V34_4D_ENCODER_OK &&
			      v34_precoder_init(
				      &inverse_precoder, &parameters,
				      COEFFICIENTS) == V34_PRECODER_OK &&
			      v34_4d_decoder_init(
				      &mapping_decoder, &parameters) ==
				      V34_4D_DECODER_OK &&
			      inverse_precoder.grid_step ==
				      PROFILES[profile_index].grid_step,
			      "path mode %zu/%u did not initialize", profile_index,
			      selection);
			for (size_t interval = 0;
			     interval < GENERATED_INTERVALS; ++interval) {
				struct v34_4d_encoder_input input =
					generated_input(
						&parameters, selection, interval);
				struct v34_4d_encoder_output encoded;
				struct v34_survivor_decision survivor;
				struct v34_decided_4d_interval decided;
				struct v34_decoded_4d_interval decoded;

				CHECK(v34_4d_encoder_step(
					      &encoder, &input, &encoded) ==
					      V34_4D_ENCODER_OK,
				      "path encode %zu/%u/%zu failed",
				      profile_index, selection, interval);
				survivor = survivor_decision(
					&encoded, input.inversion_bit);
				CHECK(v34_survivor_decision_is_valid(
					      &survivor) &&
				      v34_decode_survivor_path(
					      &inverse_precoder, &survivor,
					      &decided) ==
					      V34_PATH_DECODER_OK &&
				      decided.trellis_output ==
					      encoded.trellis_output &&
				      grid_points_are_equal(
					      &decided.mapper_output[0],
					      &encoded.mapped[0].mapper_output) &&
				      grid_points_are_equal(
					      &decided.mapper_output[1],
					      &encoded.mapped[1].mapper_output) &&
				      precoders_are_equal(
					      &inverse_precoder,
					      &encoder.precoder) &&
				      v34_decode_4d_interval(
					      &mapping_decoder, &decided,
					      &decoded) ==
					      V34_4D_DECODER_OK &&
				      decoded_matches(
					      &decoded, &input,
					      encoded.differential_output),
				      "path decode %zu/%u/%zu changed",
				      profile_index, selection, interval);
				modulo_seen[encoded.modulo_output] = true;
				convolutional_seen[encoded.convolutional_output] = true;
				inversion_seen[input.inversion_bit] = true;
				trellis_seen[encoded.trellis_output] = true;
				for (uint8_t symbol = 0;
				     symbol <
					     V34_2D_SYMBOLS_PER_4D_INTERVAL;
				     ++symbol) {
					if (encoded.precoded[symbol]
						    .lattice_offset.in_phase != 0 ||
					    encoded.precoded[symbol]
						    .lattice_offset.quadrature != 0)
						nonzero_offset_seen[profile_index] =
							true;
				}
			}
		}
	}
	CHECK(modulo_seen[0] && modulo_seen[1] && convolutional_seen[0] &&
	      convolutional_seen[1] && inversion_seen[0] && inversion_seen[1] &&
	      trellis_seen[0] && trellis_seen[1] && nonzero_offset_seen[0] &&
	      nonzero_offset_seen[1],
	      "generated path did not cover its decision terms");
	return true;
}

static bool rejected_path_is_transactional(
	struct v34_precoder *precoder,
	const struct v34_survivor_decision *survivor,
	enum v34_path_decoder_result expected)
{
	struct v34_precoder saved = *precoder;
	struct v34_decided_4d_interval decided;
	struct v34_decided_4d_interval saved_decided;

	memset(&decided, 0xa5, sizeof(decided));
	saved_decided = decided;
	return v34_decode_survivor_path(
		       precoder, survivor, &decided) == expected &&
	       memcmp(precoder, &saved, sizeof(saved)) == 0 &&
	       memcmp(&decided, &saved_decided, sizeof(decided)) == 0;
}

static bool test_transactional_rejection(void)
{
	static const struct v34_q14_coefficient zero_coefficients
		[V34_PRECODER_TAP_COUNT] = { 0 };
	struct v34_mapping_parameters parameters;
	struct v34_precoder precoder;
	struct v34_survivor_decision decision = {
		.channel_output = { { 1, 1 }, { 1, 1 } },
	};
	struct v34_survivor_decision malformed;
	struct v34_decided_4d_interval decided;
	struct v34_decided_4d_interval saved_decided;
	struct v34_grid_point recovered;
	struct v34_precoder_symbol recovered_symbol;

	CHECK(initialize_parameters(&PROFILES[0], &parameters) &&
	      v34_precoder_init(
		      &precoder, &parameters, zero_coefficients) ==
		      V34_PRECODER_OK &&
	      v34_survivor_decision_is_valid(&decision) &&
	      !v34_survivor_decision_is_valid(NULL),
	      "path rejection fixture did not initialize");
	memset(&decided, 0xa5, sizeof(decided));
	saved_decided = decided;
	CHECK(v34_decode_survivor_path(
		      NULL, &decision, &decided) ==
		      V34_PATH_DECODER_INVALID_ARGUMENT &&
	      v34_decode_survivor_path(
		      &precoder, NULL, &decided) ==
		      V34_PATH_DECODER_INVALID_ARGUMENT &&
	      v34_decode_survivor_path(
		      &precoder, &decision, NULL) ==
		      V34_PATH_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decided, &saved_decided, sizeof(decided)) == 0,
	      "null path arguments changed their output");
	malformed = decision;
	malformed.convolutional_output = 1;
	CHECK(!v34_survivor_decision_is_valid(&malformed) &&
	      rejected_path_is_transactional(
		      &precoder, &malformed,
		      V34_PATH_DECODER_INVALID_ARGUMENT),
	      "inconsistent survivor parity was accepted");
	malformed = decision;
	malformed.inversion_bit = 2;
	CHECK(!v34_survivor_decision_is_valid(&malformed) &&
	      rejected_path_is_transactional(
		      &precoder, &malformed,
		      V34_PATH_DECODER_INVALID_ARGUMENT),
	      "invalid survivor inversion was accepted");
	malformed = decision;
	malformed.channel_output[1].in_phase = 0;
	CHECK(!v34_survivor_decision_is_valid(&malformed) &&
	      rejected_path_is_transactional(
		      &precoder, &malformed,
		      V34_PATH_DECODER_INVALID_ARGUMENT),
	      "off-grid survivor point was accepted");

	precoder = (struct v34_precoder){
		.coefficients = {
			{ INT16_MAX, 0 }, { INT16_MAX, 0 }, { 0, 0 },
		},
		.previous_symbols = {
			{ 1000000000, 0 }, { -1000000000, 0 }, { 0, 0 },
		},
		.grid_step = V34_PRECODER_GRID_STEP_2,
	};
	{
		struct v34_precoder probe = precoder;

		CHECK(v34_precoder_inverse_step(
			      &probe, &decision.channel_output[0], &recovered,
			      &recovered_symbol) == V34_PRECODER_OK &&
		      v34_precoder_inverse_step(
			      &probe, &decision.channel_output[1], &recovered,
			      &recovered_symbol) ==
			      V34_PRECODER_OUT_OF_RANGE,
		      "late-failure fixture did not cross the second symbol");
	}
	CHECK(rejected_path_is_transactional(
		      &precoder, &decision,
		      V34_PATH_DECODER_OUT_OF_RANGE),
	      "late inverse-precoder failure changed path state");
	precoder.grid_step = (enum v34_precoder_grid_step)3;
	CHECK(rejected_path_is_transactional(
		      &precoder, &decision,
		      V34_PATH_DECODER_INVALID_ARGUMENT),
	      "invalid inverse-precoder state was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "known path", test_known_path },
		{ "generated round trip", test_generated_round_trip },
		{ "transactional rejection", test_transactional_rejection },
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
