// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_decoder.h>

#include "test.h"

#include <string.h>

struct symbol_rate_fixture {
	enum v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ V34_SYMBOL_RATE_2400, 1, 9 },
	{ V34_SYMBOL_RATE_2743, 2, 11 },
	{ V34_SYMBOL_RATE_2800, 2, 11 },
	{ V34_SYMBOL_RATE_3000, 2, 12 },
	{ V34_SYMBOL_RATE_3200, 2, 13 },
	{ V34_SYMBOL_RATE_3429, 2, 14 },
};

static bool initialize_parameters(
	enum v34_symbol_rate symbol_rate, uint8_t rate,
	bool auxiliary, bool expanded,
	struct v34_mapping_parameters *parameters)
{
	struct v34_frame_geometry geometry;

	return v34_frame_geometry_init(
		       &geometry, symbol_rate, rate, auxiliary) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       parameters, &geometry, expanded) ==
		       V34_MAPPING_PARAMETERS_OK;
}

static bool generate_decision(
	const struct v34_mapping_parameters *parameters,
	uint8_t previous_differential_output,
	const struct v34_4d_mapping_input *mapping,
	const uint8_t ring_index[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t trellis_output,
	struct v34_decided_4d_interval *decided)
{
	uint8_t differential_output = (uint8_t)(
		(previous_differential_output + mapping->differential_input) &
		3u);
	uint8_t rotations[2] = {
		differential_output,
		(uint8_t)((differential_output + 2u * mapping->rotation_bit +
			   trellis_output) &
			  3u),
	};

	decided->trellis_output = trellis_output;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct v34_2d_mapping_input input = {
			.ring_index = ring_index[symbol],
			.point_index = mapping->point_index[symbol],
			.clockwise_quarter_turns = rotations[symbol],
		};
		struct v34_2d_mapping_output output;

		if (v34_map_2d_symbol(
			    parameters, &input, &output) !=
		    V34_MAPPER_OK)
			return false;
		decided->mapper_output[symbol] = output.mapper_output;
	}
	return true;
}

static bool decoded_matches(
	const struct v34_decoded_4d_interval *decoded,
	const struct v34_4d_mapping_input *mapping,
	const uint8_t ring_index[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t differential_output)
{
	return decoded->mapping.rotation_bit == mapping->rotation_bit &&
	       decoded->mapping.differential_input ==
		       mapping->differential_input &&
	       memcmp(
		       decoded->mapping.point_index, mapping->point_index,
		       sizeof(decoded->mapping.point_index)) == 0 &&
	       memcmp(
		       decoded->ring_index, ring_index,
		       sizeof(decoded->ring_index)) == 0 &&
	       decoded->differential_output == differential_output;
}

static struct v34_grid_point rotate_clockwise(
	struct v34_grid_point point, uint8_t quarter_turns)
{
	for (uint8_t turn = 0; turn < quarter_turns; ++turn) {
		int16_t saved_in_phase = point.in_phase;

		point.in_phase = point.quadrature;
		point.quadrature = (int16_t)-saved_in_phase;
	}
	return point;
}

static bool test_all_supported_sequences(void)
{
	bool point_width_seen[6] = { false };
	bool differential_seen[4] = { false };
	bool rotation_seen[2] = { false };
	bool trellis_seen[2] = { false };
	size_t configuration_count = 0;
	size_t interval_count = 0;

	for (size_t rate_index = 0; rate_index < ARRAY_LENGTH(SYMBOL_RATES);
	     ++rate_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[rate_index];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (uint8_t auxiliary = 0; auxiliary < 2u; ++auxiliary) {
				for (uint8_t expanded = 0; expanded < 2u; ++expanded) {
					struct v34_mapping_parameters parameters;
					struct v34_4d_decoder decoder;
					uint8_t previous = 0;
					uint8_t point_limit;

					CHECK(initialize_parameters(
						      fixture->symbol_rate, rate,
						      auxiliary != 0u, expanded != 0u,
						      &parameters) &&
					      v34_4d_decoder_init(
						      &decoder, &parameters) ==
						      V34_4D_DECODER_OK,
					      "decoder configuration %zu/%u/%u/%u failed",
					      rate_index, rate, auxiliary, expanded);
					point_limit = (uint8_t)(
						1u << parameters
							      .point_index_bits_per_2d_symbol);
					point_width_seen[parameters
							 .point_index_bits_per_2d_symbol] = true;
					configuration_count++;
					for (uint8_t interval = 0; interval < 8u;
					     ++interval) {
						struct v34_4d_mapping_input mapping = {
							.rotation_bit =
								(uint8_t)((interval + rate) & 1u),
							.differential_input = (uint8_t)(
								(interval + rate + expanded) & 3u),
							.point_index = {
								(uint8_t)((3u * interval + rate) %
									  point_limit),
								(uint8_t)((5u * interval + auxiliary) %
									  point_limit),
							},
						};
						uint8_t ring_index[2] = {
							(uint8_t)((interval + rate_index) %
								  parameters.ring_count),
							(uint8_t)((3u * interval + rate) %
								  parameters.ring_count),
						};
						uint8_t trellis_output =
							(uint8_t)((interval + auxiliary) & 1u);
						uint8_t current = (uint8_t)(
							(previous + mapping.differential_input) & 3u);
						struct v34_decided_4d_interval decided;
						struct v34_decoded_4d_interval decoded;

						CHECK(generate_decision(
							      &parameters, previous, &mapping,
							      ring_index, trellis_output, &decided) &&
						      v34_decode_4d_interval(
							      &decoder, &decided, &decoded) ==
							      V34_4D_DECODER_OK &&
						      decoded_matches(
							      &decoded, &mapping, ring_index, current) &&
						      decoder.previous_differential_output == current,
						      "decoder sequence %zu/%u/%u/%u/%u changed",
						      rate_index, rate, auxiliary, expanded,
						      interval);
						differential_seen[mapping.differential_input] = true;
						rotation_seen[mapping.rotation_bit] = true;
						trellis_seen[trellis_output] = true;
						previous = current;
						interval_count++;
					}
				}
			}
		}
	}
	CHECK(configuration_count == 260u && interval_count == 2080u &&
	      differential_seen[0] && differential_seen[1] &&
	      differential_seen[2] && differential_seen[3] &&
	      rotation_seen[0] && rotation_seen[1] &&
	      trellis_seen[0] && trellis_seen[1],
	      "decoder sequence coverage changed");
	for (size_t width = 0; width < ARRAY_LENGTH(point_width_seen); ++width)
		CHECK(point_width_seen[width],
		      "point-index width %zu was not decoded", width);
	return true;
}

static bool decoders_are_equal(
	const struct v34_4d_decoder *left,
	const struct v34_4d_decoder *right)
{
	return memcmp(
		       &left->parameters, &right->parameters,
		       sizeof(left->parameters)) == 0 &&
	       left->previous_differential_output ==
		       right->previous_differential_output;
}

static bool rejection_is_transactional(
	struct v34_4d_decoder *decoder,
	const struct v34_decided_4d_interval *decided)
{
	struct v34_4d_decoder saved = *decoder;
	struct v34_decoded_4d_interval decoded;
	struct v34_decoded_4d_interval decoded_saved;

	memset(&decoded, 0xa5, sizeof(decoded));
	decoded_saved = decoded;
	return v34_decode_4d_interval(
		       decoder, decided, &decoded) ==
		       V34_4D_DECODER_INVALID_ARGUMENT &&
	       decoders_are_equal(decoder, &saved) &&
	       memcmp(&decoded, &decoded_saved, sizeof(decoded)) == 0;
}

static bool test_validation(void)
{
	struct v34_mapping_parameters parameters;
	struct v34_mapping_parameters invalid_parameters;
	struct v34_4d_decoder decoder;
	struct v34_4d_decoder saved;
	struct v34_4d_decoder invalid;
	struct v34_4d_mapping_input mapping = { 0 };
	uint8_t ring_index[2] = { 0 };
	struct v34_decided_4d_interval decided;
	struct v34_decided_4d_interval malformed;
	struct v34_decoded_4d_interval decoded;
	struct v34_2d_mapping_input rotated = {
		.clockwise_quarter_turns = 1,
	};
	struct v34_2d_mapping_output rotated_output;

	CHECK(initialize_parameters(
		      V34_SYMBOL_RATE_2400, 1, false, false,
		      &parameters) &&
	      v34_4d_decoder_init(&decoder, &parameters) ==
		      V34_4D_DECODER_OK &&
	      generate_decision(
		      &parameters, 0, &mapping, ring_index, 0, &decided),
	      "decoder validation fixture did not initialize");
	saved = decoder;
	invalid_parameters = parameters;
	invalid_parameters.ring_count++;
	CHECK(v34_4d_decoder_init(NULL, &parameters) ==
		      V34_4D_DECODER_INVALID_ARGUMENT &&
	      v34_4d_decoder_init(&decoder, NULL) ==
		      V34_4D_DECODER_INVALID_ARGUMENT &&
	      v34_4d_decoder_init(
		      &decoder, &invalid_parameters) ==
		      V34_4D_DECODER_INVALID_ARGUMENT &&
	      decoders_are_equal(&decoder, &saved),
	      "invalid decoder initialization changed its destination");
	CHECK(v34_4d_decoder_is_valid(&decoder) &&
	      !v34_4d_decoder_is_valid(NULL) &&
	      v34_decode_4d_interval(NULL, &decided, &decoded) ==
		      V34_4D_DECODER_INVALID_ARGUMENT &&
	      v34_decode_4d_interval(&decoder, NULL, &decoded) ==
		      V34_4D_DECODER_INVALID_ARGUMENT &&
	      v34_decode_4d_interval(&decoder, &decided, NULL) ==
		      V34_4D_DECODER_INVALID_ARGUMENT,
	      "decoder accepted an invalid boundary argument");

	malformed = decided;
	malformed.trellis_output = 2;
	CHECK(rejection_is_transactional(&decoder, &malformed),
	      "invalid trellis output changed decoder state");
	malformed = decided;
	malformed.mapper_output[0].in_phase++;
	CHECK(rejection_is_transactional(&decoder, &malformed),
	      "off-grid mapper output changed decoder state");
	CHECK(v34_map_2d_symbol(
		      &parameters, &rotated, &rotated_output) ==
		      V34_MAPPER_OK,
	      "inconsistent-rotation fixture did not map");
	malformed = decided;
	malformed.mapper_output[1] = rotated_output.mapper_output;
	CHECK(rejection_is_transactional(&decoder, &malformed),
	      "inconsistent 4D rotation changed decoder state");
	invalid = decoder;
	invalid.previous_differential_output = 4;
	CHECK(!v34_4d_decoder_is_valid(&invalid) &&
	      rejection_is_transactional(&invalid, &decided),
	      "invalid differential state was accepted");
	invalid = decoder;
	invalid.parameters = invalid_parameters;
	CHECK(!v34_4d_decoder_is_valid(&invalid) &&
	      rejection_is_transactional(&invalid, &decided),
	      "invalid mapping parameters were accepted");
	CHECK(strcmp(v34_4d_decoder_result_name(
		     V34_4D_DECODER_OK),
		     "ok") == 0 &&
	      strcmp(v34_4d_decoder_result_name(
		     V34_4D_DECODER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_4d_decoder_result_name(
		     (enum v34_4d_decoder_result)2),
		     "unknown") == 0,
	      "decoder result names changed");
	return true;
}

static bool test_noisy_decision_folding(void)
{
	struct v34_mapping_parameters parameters;
	struct v34_4d_decoder decoder;
	const struct v34_4d_mapping_input mapping = {
		.rotation_bit = 1,
		.differential_input = 1,
	};
	const uint8_t ring_index[2] = { 0 };
	struct v34_decided_4d_interval decided;
	struct v34_decoded_4d_interval decoded;
	struct v34_grid_point outside;

	CHECK(initialize_parameters(
		      V34_SYMBOL_RATE_2400, 1, false, false,
		      &parameters) &&
	      v34_4d_decoder_init(&decoder, &parameters) ==
		      V34_4D_DECODER_OK &&
	      generate_decision(
		      &parameters, 0, &mapping, ring_index, 0, &decided) &&
	      v34_quarter_superconstellation_point(
		      1, &outside) == V34_CONSTELLATION_OK,
	      "noisy-decision fixture did not initialize");
	decided.mapper_output[0] = rotate_clockwise(outside, 1);
	decided.mapper_output[1] = rotate_clockwise(outside, 3);
	CHECK(v34_decode_4d_interval(
		      &decoder, &decided, &decoded) == V34_4D_DECODER_OK &&
	      decoded_matches(&decoded, &mapping, ring_index, 1),
	      "outside-prefix hard decision did not preserve the 4-D mapping");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all supported sequences", test_all_supported_sequences },
		{ "noisy decision folding", test_noisy_decision_folding },
		{ "validation", test_validation },
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
