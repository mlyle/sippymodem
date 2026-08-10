// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_parser.h>

#include "test.h"

#include <string.h>

enum {
	MAX_MAPPING_FRAME_BITS = 79,
};

struct symbol_rate_fixture {
	enum sippymodem_v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, 9 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2743, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3000, 2, 12 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3200, 2, 13 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3429, 2, 14 },
};

static bool parsed_frames_are_equal(
	const struct sippymodem_v34_parsed_mapping_frame *left,
	const struct sippymodem_v34_parsed_mapping_frame *right)
{
	if (left->shell_label != right->shell_label)
		return false;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		if (left->interval[interval].rotation_bit !=
			    right->interval[interval].rotation_bit ||
		    left->interval[interval].differential_input !=
			    right->interval[interval].differential_input)
			return false;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (left->interval[interval].point_index[symbol] !=
			    right->interval[interval].point_index[symbol])
				return false;
		}
	}
	return true;
}

static bool initialize_parameters(
	enum sippymodem_v34_symbol_rate symbol_rate, uint8_t primary_rate,
	bool auxiliary, bool expanded,
	struct sippymodem_v34_mapping_parameters *parameters)
{
	struct sippymodem_v34_frame_geometry geometry;

	return sippymodem_v34_frame_geometry_init(
		       &geometry, symbol_rate, primary_rate, auxiliary) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	       sippymodem_v34_mapping_parameters_init(
		       parameters, &geometry, expanded) ==
		       SIPPYMODEM_V34_MAPPING_PARAMETERS_OK;
}

static bool test_regular_parser_layout(void)
{
	static const uint8_t primary_bits[] = {
		1, 0, 1, 1,
		0, 1, 1,
		1, 0, 1,
		1, 1, 0,
		0, 0, 1,
	};
	static const struct sippymodem_v34_parsed_mapping_frame expected = {
		.shell_label = 13,
		.interval = {
			{ .rotation_bit = 0, .differential_input = 3 },
			{ .rotation_bit = 1, .differential_input = 2 },
			{ .rotation_bit = 1, .differential_input = 1 },
			{ .rotation_bit = 0, .differential_input = 2 },
		},
	};
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_mapping_frame mapping_frame = {
		.data_bits = ARRAY_LENGTH(primary_bits),
		.primary_bits = ARRAY_LENGTH(primary_bits),
		.high_frame = true,
	};
	struct sippymodem_v34_parsed_mapping_frame parsed;

	CHECK(initialize_parameters(
		      SIPPYMODEM_V34_SYMBOL_RATE_2400, 2, false, false,
		      &parameters) &&
	      parameters.mapping_frame_bits == 16u &&
	      parameters.shell_mapper_bits == 4u &&
	      parameters.point_index_bits_per_2d_symbol == 0u,
	      "regular parser fixture has unexpected dimensions");
	CHECK(sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), NULL, &parsed) ==
		      SIPPYMODEM_V34_PARSER_OK &&
	      parsed_frames_are_equal(&parsed, &expected) &&
	      sippymodem_v34_parsed_mapping_frame_is_valid(
		      &parsed, &parameters, &mapping_frame),
	      "regular parser changed shell or 4D bit order");

	CHECK(initialize_parameters(
		      SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, false, false,
		      &parameters) &&
	      parameters.mapping_frame_bits == 14u &&
	      parameters.shell_mapper_bits == 2u,
	      "low-frame parser fixture has unexpected dimensions");
	mapping_frame = (struct sippymodem_v34_mapping_frame){
		.data_bits = 13,
		.primary_bits = 13,
		.high_frame = false,
	};
	CHECK(sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits, 13, NULL,
		      &parsed) == SIPPYMODEM_V34_PARSER_OK &&
	      parsed.shell_label == 1u &&
	      parsed.interval[0].rotation_bit == 0u &&
	      parsed.interval[0].differential_input == 3u,
	      "low frame did not append the zero shell bit");

	CHECK(initialize_parameters(
		      SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, true, false,
		      &parameters) &&
	      parameters.mapping_frame_bits == 15u &&
	      parameters.shell_mapper_bits == 3u,
	      "auxiliary parser fixture has unexpected dimensions");
	mapping_frame = (struct sippymodem_v34_mapping_frame){
		.data_bits = 15,
		.primary_bits = 14,
		.high_frame = true,
		.carries_auxiliary_bit = true,
	};
	{
		const uint8_t auxiliary_bit = 1;

		CHECK(sippymodem_v34_parse_mapping_frame(
			      &parameters, &mapping_frame, primary_bits, 14,
			      &auxiliary_bit, &parsed) ==
			      SIPPYMODEM_V34_PARSER_OK &&
		      parsed.shell_label == 5u &&
		      parsed.interval[0].rotation_bit == auxiliary_bit,
		      "auxiliary bit did not replace interval zero I1");
	}
	return true;
}

static bool test_short_parser_layouts(void)
{
	static const struct {
		enum sippymodem_v34_symbol_rate symbol_rate;
		uint8_t primary_rate;
		bool geometry_auxiliary;
		bool high_frame;
		bool carries_auxiliary;
		uint8_t auxiliary_bit;
		uint8_t data_bits;
		uint8_t primary_bits[12];
		uint8_t rotation[4];
		uint8_t differential[4];
	} cases[] = {
		{
			SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, false, true,
			false, 0, 8,
			{ 1, 0, 0, 1, 1, 1, 0, 0 },
			{ 1, 0, 1, 0 }, { 0, 1, 1, 0 },
		},
		{
			SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, true, true,
			true, 1, 9,
			{ 0, 1, 1, 0, 0, 1, 1, 1 },
			{ 1, 1, 0, 1 }, { 2, 0, 1, 1 },
		},
		{
			SIPPYMODEM_V34_SYMBOL_RATE_3429, 2, false, false,
			false, 0, 11,
			{ 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1 },
			{ 1, 0, 1, 0 }, { 2, 3, 1, 1 },
		},
		{
			SIPPYMODEM_V34_SYMBOL_RATE_3429, 2, false, true,
			false, 0, 12,
			{ 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0 },
			{ 0, 1, 0, 1 }, { 1, 2, 3, 1 },
		},
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct sippymodem_v34_mapping_parameters parameters;
		struct sippymodem_v34_mapping_frame mapping_frame = {
			.data_bits = cases[index].data_bits,
			.primary_bits = (uint8_t)(
				cases[index].data_bits -
				cases[index].carries_auxiliary),
			.high_frame = cases[index].high_frame,
			.carries_auxiliary_bit = cases[index].carries_auxiliary,
		};
		struct sippymodem_v34_parsed_mapping_frame parsed;
		const uint8_t *auxiliary_bit = cases[index].carries_auxiliary ?
			&cases[index].auxiliary_bit : NULL;

		CHECK(initialize_parameters(
			      cases[index].symbol_rate,
			      cases[index].primary_rate,
			      cases[index].geometry_auxiliary, false,
			      &parameters) &&
		      parameters.mapping_frame_bits <= 12u,
		      "short parser fixture %zu has unexpected dimensions",
		      index);
		CHECK(sippymodem_v34_parse_mapping_frame(
			      &parameters, &mapping_frame,
			      cases[index].primary_bits,
			      mapping_frame.primary_bits, auxiliary_bit,
			      &parsed) == SIPPYMODEM_V34_PARSER_OK,
		      "short parser fixture %zu was rejected", index);
		CHECK(parsed.shell_label == 0u,
		      "short parser fixture %zu generated a shell label", index);
		for (uint8_t interval = 0;
		     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
		     ++interval) {
			CHECK(parsed.interval[interval].rotation_bit ==
				      cases[index].rotation[interval] &&
			      parsed.interval[interval].differential_input ==
				      cases[index].differential[interval] &&
			      parsed.interval[interval].point_index[0] == 0u &&
			      parsed.interval[interval].point_index[1] == 0u,
			      "short parser fixture %zu interval %u changed", index,
			      interval);
		}
	}
	return true;
}

static bool append_bit(
	uint8_t *bits, size_t capacity, size_t *count, uint8_t bit)
{
	if (*count >= capacity || bit > 1u)
		return false;
	bits[(*count)++] = bit;
	return true;
}

static bool append_value(
	uint8_t *bits, size_t capacity, size_t *count, uint32_t value,
	uint8_t bit_count)
{
	for (uint8_t index = 0; index < bit_count; ++index) {
		if (!append_bit(
			    bits, capacity, count,
			    (uint8_t)((value >> index) & 1u)))
			return false;
	}
	return true;
}

static uint8_t short_i3_count(uint8_t data_bits)
{
	switch (data_bits) {
	case 8:
		return 0;
	case 9:
		return 1;
	case 11:
		return 3;
	case 12:
		return 4;
	}
	return UINT8_MAX;
}

static bool reconstruct_primary_bits(
	const struct sippymodem_v34_parsed_mapping_frame *parsed,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const uint8_t *auxiliary_bit, uint8_t *reconstructed,
	size_t *reconstructed_count)
{
	size_t count = 0;
	uint8_t i3_count = parameters->mapping_frame_bits <= 12u ?
		short_i3_count(mapping_frame->data_bits) :
		SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	uint8_t shell_bits = parameters->mapping_frame_bits > 12u ?
		(uint8_t)(parameters->shell_mapper_bits -
			  !mapping_frame->high_frame) :
		0u;

	if (i3_count == UINT8_MAX ||
	    !append_value(
		    reconstructed, MAX_MAPPING_FRAME_BITS, &count,
		    parsed->shell_label, shell_bits))
		return false;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct sippymodem_v34_4d_mapping_input *input =
			&parsed->interval[interval];

		if (interval == 0 && mapping_frame->carries_auxiliary_bit) {
			if (auxiliary_bit == NULL ||
			    input->rotation_bit != *auxiliary_bit)
				return false;
		} else if (!append_bit(
				   reconstructed, MAX_MAPPING_FRAME_BITS, &count,
				   input->rotation_bit)) {
			return false;
		}
		if (!append_bit(
			    reconstructed, MAX_MAPPING_FRAME_BITS, &count,
			    input->differential_input & 1u) ||
		    (interval < i3_count &&
		     !append_bit(
			     reconstructed, MAX_MAPPING_FRAME_BITS, &count,
			     (input->differential_input >> 1) & 1u)))
			return false;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (!append_value(
				    reconstructed, MAX_MAPPING_FRAME_BITS, &count,
				    input->point_index[symbol],
				    parameters
					    ->point_index_bits_per_2d_symbol))
				return false;
		}
	}
	*reconstructed_count = count;
	return true;
}

static void generate_primary_bits(
	uint8_t *bits, size_t count, uint32_t seed)
{
	for (size_t index = 0; index < count; ++index) {
		seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(seed >> 31);
	}
}

static bool test_all_supported_schedules(void)
{
	unsigned int coverage = 0;

	for (size_t symbol_index = 0;
	     symbol_index < ARRAY_LENGTH(SYMBOL_RATES); ++symbol_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[symbol_index];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct sippymodem_v34_frame_geometry geometry;

				CHECK(sippymodem_v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0) ==
					      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
				      "parser geometry %zu/%u/%u was rejected",
				      symbol_index, rate, auxiliary);
				for (unsigned int expanded = 0; expanded < 2;
				     ++expanded) {
					struct sippymodem_v34_mapping_parameters
						parameters;
					struct sippymodem_v34_frame_schedule schedule;

					CHECK(sippymodem_v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0) ==
						      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
					      sippymodem_v34_frame_schedule_init(
						      &schedule, &geometry, 0) ==
						      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
					      "parser case %zu/%u/%u/%u did not start",
					      symbol_index, rate, auxiliary, expanded);
					while (!sippymodem_v34_frame_schedule_is_complete(
						&schedule)) {
						struct sippymodem_v34_mapping_frame frame;
						struct sippymodem_v34_parsed_mapping_frame parsed;
						uint8_t primary[MAX_MAPPING_FRAME_BITS];
						uint8_t reconstructed[MAX_MAPPING_FRAME_BITS];
						uint8_t auxiliary_bit;
						const uint8_t *auxiliary_pointer;
						size_t reconstructed_count;

						CHECK(sippymodem_v34_frame_schedule_next(
							      &schedule, &frame) ==
							      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
						      "parser frame %zu/%u/%u/%u stopped",
						      symbol_index, rate, auxiliary, expanded);
						generate_primary_bits(
							primary, frame.primary_bits,
							(uint32_t)(symbol_index * 1000u +
								   rate * 31u +
								   auxiliary * 7u +
								   expanded * 3u +
								   frame.index));
						auxiliary_bit = (uint8_t)(
							(symbol_index + rate + frame.index) & 1u);
						auxiliary_pointer =
							frame.carries_auxiliary_bit ?
								&auxiliary_bit : NULL;
						CHECK(sippymodem_v34_parse_mapping_frame(
							      &parameters, &frame, primary,
							      frame.primary_bits,
							      auxiliary_pointer, &parsed) ==
							      SIPPYMODEM_V34_PARSER_OK &&
						      sippymodem_v34_parsed_mapping_frame_is_valid(
							      &parsed, &parameters, &frame) &&
						      reconstruct_primary_bits(
							      &parsed, &parameters, &frame,
							      auxiliary_pointer, reconstructed,
							      &reconstructed_count) &&
						      reconstructed_count == frame.primary_bits &&
						      memcmp(
							      reconstructed, primary,
							      frame.primary_bits) == 0,
						      "parser round trip %zu/%u/%u/%u/%u failed",
						      symbol_index, rate, auxiliary, expanded,
						      frame.index);
						coverage |= frame.high_frame ? 1u : 2u;
						coverage |= frame.carries_auxiliary_bit ? 4u : 8u;
						if (frame.data_bits == 8u)
							coverage |= 16u;
						if (frame.data_bits == 9u)
							coverage |= 32u;
						if (frame.data_bits == 11u)
							coverage |= 64u;
						if (frame.data_bits == 12u)
							coverage |= 128u;
						if (parameters
							    .point_index_bits_per_2d_symbol > 0u)
							coverage |= 256u;
					}
				}
			}
		}
	}
	CHECK(coverage == 0x1ffu,
	      "exhaustive parser cases missed coverage mask 0x%x", coverage);
	return true;
}

static bool test_validation(void)
{
	uint8_t primary_bits[14] = { 0 };
	uint8_t auxiliary_bit = 1;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_mapping_parameters corrupt_parameters;
	struct sippymodem_v34_mapping_frame mapping_frame = {
		.data_bits = 15,
		.primary_bits = 14,
		.high_frame = true,
		.carries_auxiliary_bit = true,
	};
	struct sippymodem_v34_mapping_frame corrupt_frame;
	struct sippymodem_v34_parsed_mapping_frame parsed;
	struct sippymodem_v34_parsed_mapping_frame saved;

	CHECK(initialize_parameters(
		      SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, true, false,
		      &parameters) &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_OK,
	      "parser validation fixture did not initialize");
	saved = parsed;
	CHECK(sippymodem_v34_parse_mapping_frame(
		      NULL, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, NULL, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, NULL) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, NULL,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits) - 1u, &auxiliary_bit,
		      &parsed) == SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), NULL, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      parsed_frames_are_equal(&parsed, &saved),
	      "invalid parser arguments changed the destination");

	primary_bits[5] = 2;
	CHECK(sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      parsed_frames_are_equal(&parsed, &saved),
	      "parser accepted a non-bit primary value");
	primary_bits[5] = 0;
	auxiliary_bit = 2;
	CHECK(sippymodem_v34_parse_mapping_frame(
		      &parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      parsed_frames_are_equal(&parsed, &saved),
	      "parser accepted a non-bit auxiliary value");
	auxiliary_bit = 1;

	corrupt_parameters = parameters;
	corrupt_parameters.ring_count++;
	corrupt_frame = mapping_frame;
	corrupt_frame.data_bits--;
	CHECK(sippymodem_v34_parse_mapping_frame(
		      &corrupt_parameters, &mapping_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      sippymodem_v34_parse_mapping_frame(
		      &parameters, &corrupt_frame, primary_bits,
		      ARRAY_LENGTH(primary_bits), &auxiliary_bit, &parsed) ==
		      SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT &&
	      parsed_frames_are_equal(&parsed, &saved),
	      "parser accepted corrupt parameters or schedule");

	parsed = saved;
	parsed.shell_label = 1u << parameters.shell_mapper_bits;
	CHECK(!sippymodem_v34_parsed_mapping_frame_is_valid(
		      &parsed, &parameters, &mapping_frame),
	      "parsed frame accepted an oversized shell label");
	parsed = saved;
	parsed.interval[0].rotation_bit = 2;
	CHECK(!sippymodem_v34_parsed_mapping_frame_is_valid(
		      &parsed, &parameters, &mapping_frame),
	      "parsed frame accepted an invalid rotation bit");
	parsed = saved;
	parsed.interval[0].differential_input = 4;
	CHECK(!sippymodem_v34_parsed_mapping_frame_is_valid(
		      &parsed, &parameters, &mapping_frame),
	      "parsed frame accepted an invalid differential input");
	parsed = saved;
	parsed.interval[0].point_index[0] = 1;
	CHECK(!sippymodem_v34_parsed_mapping_frame_is_valid(
		      &parsed, &parameters, &mapping_frame) &&
	      !sippymodem_v34_parsed_mapping_frame_is_valid(
		      NULL, &parameters, &mapping_frame) &&
	      !sippymodem_v34_parsed_mapping_frame_is_valid(
		      &saved, NULL, &mapping_frame) &&
	      !sippymodem_v34_parsed_mapping_frame_is_valid(
		      &saved, &parameters, NULL),
	      "parsed frame accepted invalid point or arguments");

	CHECK(strcmp(sippymodem_v34_parser_result_name(
		     SIPPYMODEM_V34_PARSER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_parser_result_name(
		     SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_parser_result_name(
		     (enum sippymodem_v34_parser_result)2),
		     "unknown") == 0,
	      "parser result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "regular parser layout", test_regular_parser_layout },
		{ "short parser layouts", test_short_parser_layouts },
		{ "all supported schedules", test_all_supported_schedules },
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
