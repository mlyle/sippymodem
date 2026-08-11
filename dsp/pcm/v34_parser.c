// SPDX-License-Identifier: BSD-3-Clause

#include "v34_parser.h"

#include <stddef.h>
#include <string.h>

struct primary_bit_cursor {
	const uint8_t *bits;
	size_t count;
	size_t next;
};

struct primary_bit_builder {
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS];
	size_t count;
	size_t limit;
};

static bool mapping_frame_matches_parameters(
	const struct v34_mapping_frame *mapping_frame,
	const struct v34_mapping_parameters *parameters)
{
	uint8_t expected_bits;

	if (mapping_frame == NULL ||
	    !v34_mapping_parameters_is_valid(parameters))
		return false;
	expected_bits = (uint8_t)(
		parameters->mapping_frame_bits - !mapping_frame->high_frame);
	return mapping_frame->data_bits == expected_bits &&
	       mapping_frame->primary_bits ==
		       mapping_frame->data_bits -
			       mapping_frame->carries_auxiliary_bit;
}

static bool primary_bits_are_valid(const uint8_t *bits, size_t count)
{
	if (bits == NULL && count != 0)
		return false;
	for (size_t index = 0; index < count; ++index) {
		if (bits[index] > 1u)
			return false;
	}
	return true;
}

static bool take_primary_bit(
	struct primary_bit_cursor *cursor, uint8_t *bit)
{
	if (cursor->next >= cursor->count)
		return false;
	*bit = cursor->bits[cursor->next++];
	return true;
}

static bool take_primary_value(
	struct primary_bit_cursor *cursor, uint8_t bit_count,
	uint32_t *value)
{
	uint32_t collected = 0;

	for (uint8_t index = 0; index < bit_count; ++index) {
		uint8_t bit;

		if (!take_primary_bit(cursor, &bit))
			return false;
		collected |= (uint32_t)bit << index;
	}
	*value = collected;
	return true;
}

static bool put_primary_bit(
	struct primary_bit_builder *builder, uint8_t bit)
{
	if (builder->count >= builder->limit || bit > 1u)
		return false;
	builder->bits[builder->count++] = bit;
	return true;
}

static bool put_primary_value(
	struct primary_bit_builder *builder, uint32_t value,
	uint8_t bit_count)
{
	for (uint8_t index = 0; index < bit_count; ++index) {
		if (!put_primary_bit(
			    builder, (uint8_t)((value >> index) & 1u)))
			return false;
	}
	return true;
}

static bool take_rotation_bit(
	struct primary_bit_cursor *cursor,
	const struct v34_mapping_frame *mapping_frame,
	const uint8_t *auxiliary_bit, uint8_t interval, uint8_t *bit)
{
	if (interval == 0 && mapping_frame->carries_auxiliary_bit) {
		*bit = *auxiliary_bit;
		return true;
	}
	return take_primary_bit(cursor, bit);
}

static bool parse_regular_frame(
	struct primary_bit_cursor *cursor,
	const struct v34_mapping_parameters *parameters,
	const struct v34_mapping_frame *mapping_frame,
	const uint8_t *auxiliary_bit,
	struct v34_parsed_mapping_frame *parsed)
{
	uint8_t shell_bits = (uint8_t)(
		parameters->shell_mapper_bits - !mapping_frame->high_frame);

	if (!take_primary_value(cursor, shell_bits, &parsed->shell_label))
		return false;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		struct v34_4d_mapping_input *input =
			&parsed->interval[interval];
		uint8_t i2;
		uint8_t i3;

		if (!take_rotation_bit(
			    cursor, mapping_frame, auxiliary_bit, interval,
			    &input->rotation_bit) ||
		    !take_primary_bit(cursor, &i2) ||
		    !take_primary_bit(cursor, &i3))
			return false;
		input->differential_input = (uint8_t)(i2 | (i3 << 1));
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			uint32_t point_index;

			if (!take_primary_value(
				    cursor,
				    parameters->point_index_bits_per_2d_symbol,
				    &point_index))
				return false;
			input->point_index[symbol] = (uint8_t)point_index;
		}
	}
	return true;
}

static bool special_i3_interval_count(
	uint8_t mapping_frame_bits, uint8_t *interval_count)
{
	switch (mapping_frame_bits) {
	case 8:
		*interval_count = 0;
		return true;
	case 9:
		*interval_count = 1;
		return true;
	case 11:
		*interval_count = 3;
		return true;
	case 12:
		*interval_count = 4;
		return true;
	}
	return false;
}

static bool parse_short_frame(
	struct primary_bit_cursor *cursor,
	const struct v34_mapping_frame *mapping_frame,
	const uint8_t *auxiliary_bit,
	struct v34_parsed_mapping_frame *parsed)
{
	uint8_t i3_intervals;

	if (!special_i3_interval_count(
		    mapping_frame->data_bits, &i3_intervals))
		return false;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		struct v34_4d_mapping_input *input =
			&parsed->interval[interval];
		uint8_t i2;
		uint8_t i3 = 0;

		if (!take_rotation_bit(
			    cursor, mapping_frame, auxiliary_bit, interval,
			    &input->rotation_bit) ||
		    !take_primary_bit(cursor, &i2) ||
		    (interval < i3_intervals &&
		     !take_primary_bit(cursor, &i3)))
			return false;
		input->differential_input = (uint8_t)(i2 | (i3 << 1));
	}
	return true;
}

bool v34_parsed_mapping_frame_is_valid(
	const struct v34_parsed_mapping_frame *parsed,
	const struct v34_mapping_parameters *parameters,
	const struct v34_mapping_frame *mapping_frame)
{
	uint32_t shell_limit;
	uint8_t point_limit;
	uint8_t i3_intervals =
		V34_4D_INTERVALS_PER_MAPPING_FRAME;

	if (parsed == NULL ||
	    !mapping_frame_matches_parameters(mapping_frame, parameters))
		return false;
	if (parameters->mapping_frame_bits <= 12u &&
	    !special_i3_interval_count(
		    mapping_frame->data_bits, &i3_intervals))
		return false;
	shell_limit = parameters->shell_mapper_bits == 0 ? 1u :
		UINT32_C(1) <<
			(parameters->shell_mapper_bits -
			 !mapping_frame->high_frame);
	point_limit = (uint8_t)(
		1u << parameters->point_index_bits_per_2d_symbol);
	if (parsed->shell_label >= shell_limit)
		return false;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct v34_4d_mapping_input *input =
			&parsed->interval[interval];

		if (input->rotation_bit > 1u ||
		    input->differential_input > 3u ||
		    (interval >= i3_intervals &&
		     input->differential_input > 1u))
			return false;
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (input->point_index[symbol] >= point_limit)
				return false;
		}
	}
	return true;
}

enum v34_parser_result v34_parse_mapping_frame(
	const struct v34_mapping_parameters *parameters,
	const struct v34_mapping_frame *mapping_frame,
	const uint8_t *scrambled_primary_bits,
	size_t scrambled_primary_bit_count, const uint8_t *auxiliary_bit,
	struct v34_parsed_mapping_frame *parsed)
{
	struct v34_parsed_mapping_frame generated = { 0 };
	struct primary_bit_cursor cursor = {
		.bits = scrambled_primary_bits,
		.count = scrambled_primary_bit_count,
	};
	bool auxiliary_expected;
	bool parsed_ok;

	if (!mapping_frame_matches_parameters(mapping_frame, parameters) ||
	    parsed == NULL ||
	    scrambled_primary_bit_count != mapping_frame->primary_bits ||
	    !primary_bits_are_valid(
		    scrambled_primary_bits, scrambled_primary_bit_count))
		return V34_PARSER_INVALID_ARGUMENT;
	auxiliary_expected = mapping_frame->carries_auxiliary_bit;
	if ((auxiliary_bit != NULL) != auxiliary_expected ||
	    (auxiliary_expected && *auxiliary_bit > 1u))
		return V34_PARSER_INVALID_ARGUMENT;

	if (parameters->mapping_frame_bits > 12u)
		parsed_ok = parse_regular_frame(
			&cursor, parameters, mapping_frame, auxiliary_bit,
			&generated);
	else
		parsed_ok = parse_short_frame(
			&cursor, mapping_frame, auxiliary_bit, &generated);
	if (!parsed_ok || cursor.next != cursor.count ||
	    !v34_parsed_mapping_frame_is_valid(
		    &generated, parameters, mapping_frame))
		return V34_PARSER_INVALID_ARGUMENT;
	*parsed = generated;
	return V34_PARSER_OK;
}

static bool recover_parsed_bits(
	struct primary_bit_builder *builder,
	const struct v34_mapping_parameters *parameters,
	const struct v34_mapping_frame *mapping_frame,
	const struct v34_parsed_mapping_frame *parsed,
	uint8_t *auxiliary_bit)
{
	uint8_t i3_intervals =
		V34_4D_INTERVALS_PER_MAPPING_FRAME;
	uint8_t shell_bits = 0;

	if (parameters->mapping_frame_bits > 12u) {
		shell_bits = (uint8_t)(
			parameters->shell_mapper_bits - !mapping_frame->high_frame);
	} else if (!special_i3_interval_count(
			   mapping_frame->data_bits, &i3_intervals)) {
		return false;
	}
	if (!put_primary_value(builder, parsed->shell_label, shell_bits))
		return false;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct v34_4d_mapping_input *input =
			&parsed->interval[interval];

		if (interval == 0 && mapping_frame->carries_auxiliary_bit)
			*auxiliary_bit = input->rotation_bit;
		else if (!put_primary_bit(builder, input->rotation_bit))
			return false;
		if (!put_primary_bit(
			    builder, input->differential_input & 1u) ||
		    (interval < i3_intervals &&
		     !put_primary_bit(
			     builder,
			     (input->differential_input >> 1) & 1u)))
			return false;
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (!put_primary_value(
				    builder, input->point_index[symbol],
				    parameters
					    ->point_index_bits_per_2d_symbol))
				return false;
		}
	}
	return builder->count == builder->limit;
}

enum v34_parser_result
v34_recover_mapping_frame_bits(
	const struct v34_mapping_parameters *parameters,
	const struct v34_mapping_frame *mapping_frame,
	const struct v34_parsed_mapping_frame *parsed,
	uint8_t *scrambled_primary_bits, size_t scrambled_primary_bit_count,
	uint8_t *auxiliary_bit)
{
	struct primary_bit_builder builder = {
		.limit = scrambled_primary_bit_count,
	};
	uint8_t recovered_auxiliary = 0;
	bool auxiliary_expected;

	if (!v34_parsed_mapping_frame_is_valid(
		    parsed, parameters, mapping_frame) ||
	    scrambled_primary_bits == NULL ||
	    scrambled_primary_bit_count != mapping_frame->primary_bits)
		return V34_PARSER_INVALID_ARGUMENT;
	auxiliary_expected = mapping_frame->carries_auxiliary_bit;
	if ((auxiliary_bit != NULL) != auxiliary_expected ||
	    !recover_parsed_bits(
		    &builder, parameters, mapping_frame, parsed,
		    &recovered_auxiliary))
		return V34_PARSER_INVALID_ARGUMENT;
	memcpy(scrambled_primary_bits, builder.bits, builder.count);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = recovered_auxiliary;
	return V34_PARSER_OK;
}

const char *v34_parser_result_name(
	enum v34_parser_result result)
{
	switch (result) {
	case V34_PARSER_OK:
		return "ok";
	case V34_PARSER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
