// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_mapping_parameters.h"

#include <stddef.h>

enum {
	V34_FIXED_MAPPING_BITS = 12,
	V34_MAX_SHELL_MAPPER_BITS = 31,
	V34_2D_SYMBOLS_PER_MAPPING_FRAME = 8,
	V34_CONSTELLATION_QUADRANTS = 4,
	V34_MIN_MAPPING_FRAME_BITS = 8,
	V34_MAX_MAPPING_FRAME_BITS = 79,
	V34_MAX_RING_COUNT = 18,
};

static uint64_t eighth_power(uint32_t value)
{
	uint64_t squared = (uint64_t)value * value;
	uint64_t fourth = squared * squared;

	return fourth * fourth;
}

static uint8_t minimum_ring_count(uint8_t shell_mapper_bits)
{
	uint64_t required_sequences = UINT64_C(1) << shell_mapper_bits;

	for (uint8_t rings = 1; rings <= V34_MAX_RING_COUNT; ++rings) {
		if (eighth_power(rings) >= required_sequences)
			return rings;
	}
	return 0;
}

static uint8_t expanded_ring_count(
	uint8_t shell_mapper_bits, uint8_t minimum_rings)
{
	/* Compare 1.25 * 2^(K/8) with each half-integer boundary after
	 * raising both sides to the eighth power and clearing denominators.
	 * A tie advances to the next integer, giving the required half-up
	 * rounding without floating point. */
	uint64_t target = UINT64_C(390625) << shell_mapper_bits;

	for (uint8_t rings = minimum_rings;
	     rings <= V34_MAX_RING_COUNT; ++rings) {
		uint64_t midpoint = eighth_power((uint32_t)rings * 2u + 1u) << 8;

		if (target < midpoint)
			return rings;
	}
	return 0;
}

static bool derive_parameters(
	uint8_t mapping_frame_bits, bool expanded_shaping,
	struct sippymodem_v34_mapping_parameters *parameters)
{
	uint8_t shell_mapper_bits = 0;
	uint8_t point_index_bits = 0;
	uint8_t ring_count;

	if (mapping_frame_bits < V34_MIN_MAPPING_FRAME_BITS ||
	    mapping_frame_bits > V34_MAX_MAPPING_FRAME_BITS)
		return false;
	if (mapping_frame_bits > V34_FIXED_MAPPING_BITS)
		shell_mapper_bits =
			(uint8_t)(mapping_frame_bits - V34_FIXED_MAPPING_BITS);
	while (shell_mapper_bits > V34_MAX_SHELL_MAPPER_BITS) {
		shell_mapper_bits -= V34_2D_SYMBOLS_PER_MAPPING_FRAME;
		++point_index_bits;
	}

	ring_count = minimum_ring_count(shell_mapper_bits);
	if (expanded_shaping)
		ring_count = expanded_ring_count(shell_mapper_bits, ring_count);
	if (ring_count == 0)
		return false;

	*parameters = (struct sippymodem_v34_mapping_parameters){
		.constellation_points = (uint16_t)(
			V34_CONSTELLATION_QUADRANTS * ring_count *
			(1u << point_index_bits)),
		.mapping_frame_bits = mapping_frame_bits,
		.shell_mapper_bits = shell_mapper_bits,
		.point_index_bits_per_2d_symbol = point_index_bits,
		.ring_count = ring_count,
		.expanded_shaping = expanded_shaping,
	};
	return true;
}

static bool parameters_are_equal(
	const struct sippymodem_v34_mapping_parameters *left,
	const struct sippymodem_v34_mapping_parameters *right)
{
	return left->constellation_points == right->constellation_points &&
	       left->mapping_frame_bits == right->mapping_frame_bits &&
	       left->shell_mapper_bits == right->shell_mapper_bits &&
	       left->point_index_bits_per_2d_symbol ==
		       right->point_index_bits_per_2d_symbol &&
	       left->ring_count == right->ring_count &&
	       left->expanded_shaping == right->expanded_shaping;
}

enum sippymodem_v34_mapping_parameters_result
sippymodem_v34_mapping_parameters_init(
	struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping)
{
	struct sippymodem_v34_mapping_parameters derived;

	if (parameters == NULL ||
	    !sippymodem_v34_frame_geometry_is_valid(geometry) ||
	    !derive_parameters(
		    geometry->high_mapping_frame_bits, expanded_shaping,
		    &derived))
		return SIPPYMODEM_V34_MAPPING_PARAMETERS_INVALID_ARGUMENT;
	*parameters = derived;
	return SIPPYMODEM_V34_MAPPING_PARAMETERS_OK;
}

bool sippymodem_v34_mapping_parameters_is_valid(
	const struct sippymodem_v34_mapping_parameters *parameters)
{
	struct sippymodem_v34_mapping_parameters derived;

	return parameters != NULL &&
	       derive_parameters(
		       parameters->mapping_frame_bits,
		       parameters->expanded_shaping, &derived) &&
	       parameters_are_equal(parameters, &derived);
}

const char *sippymodem_v34_mapping_parameters_result_name(
	enum sippymodem_v34_mapping_parameters_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_PARAMETERS_OK:
		return "ok";
	case SIPPYMODEM_V34_MAPPING_PARAMETERS_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
