// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapper.h"

#include <limits.h>
#include <stddef.h>

bool sippymodem_v34_2d_mapping_input_is_valid(
	const struct sippymodem_v34_2d_mapping_input *input,
	const struct sippymodem_v34_mapping_parameters *parameters)
{
	uint16_t point_index_limit;

	if (input == NULL ||
	    !sippymodem_v34_mapping_parameters_is_valid(parameters))
		return false;
	point_index_limit = (uint16_t)(
		1u << parameters->point_index_bits_per_2d_symbol);
	return input->ring_index < parameters->ring_count &&
	       input->point_index < point_index_limit &&
	       input->clockwise_quarter_turns < 4u;
}

static bool rotate_clockwise(
	const struct sippymodem_v34_grid_point *point, uint8_t quarter_turns,
	struct sippymodem_v34_grid_point *rotated)
{
	int in_phase = point->in_phase;
	int quadrature = point->quadrature;

	if (quarter_turns >= 4u)
		return false;
	for (uint8_t turn = 0; turn < quarter_turns; ++turn) {
		int saved_in_phase = in_phase;

		in_phase = quadrature;
		quadrature = -saved_in_phase;
	}
	if (in_phase < INT16_MIN || in_phase > INT16_MAX ||
	    quadrature < INT16_MIN || quadrature > INT16_MAX)
		return false;
	*rotated = (struct sippymodem_v34_grid_point){
		.in_phase = (int16_t)in_phase,
		.quadrature = (int16_t)quadrature,
	};
	return true;
}

enum sippymodem_v34_mapper_result sippymodem_v34_map_2d_symbol(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_2d_mapping_input *input,
	struct sippymodem_v34_2d_mapping_output *output)
{
	struct sippymodem_v34_2d_mapping_output generated;
	struct sippymodem_v34_grid_point quarter_point;
	uint16_t label;

	if (!sippymodem_v34_2d_mapping_input_is_valid(input, parameters) ||
	    output == NULL)
		return SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT;
	label = (uint16_t)(
		((uint16_t)input->ring_index <<
		 parameters->point_index_bits_per_2d_symbol) |
		input->point_index);
	if (label >= parameters->constellation_points / 4u ||
	    sippymodem_v34_quarter_superconstellation_point(
		    label, &quarter_point) !=
		    SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT;
	generated.constellation_label = label;
	if (!rotate_clockwise(
		    &quarter_point, input->clockwise_quarter_turns,
		    &generated.mapper_output))
		return SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT;
	*output = generated;
	return SIPPYMODEM_V34_MAPPER_OK;
}

static bool recover_clockwise_quarter_turns(
	const struct sippymodem_v34_grid_point *point, uint8_t *quarter_turns)
{
	uint8_t in_phase_residue;
	uint8_t quadrature_residue;
	uint8_t recovered;

	if (point == NULL || quarter_turns == NULL)
		return false;
	in_phase_residue = (uint8_t)((uint16_t)point->in_phase & 3u);
	quadrature_residue = (uint8_t)((uint16_t)point->quadrature & 3u);
	if ((in_phase_residue != 1u && in_phase_residue != 3u) ||
	    (quadrature_residue != 1u && quadrature_residue != 3u))
		return false;
	if (in_phase_residue == 1u)
		recovered = quadrature_residue == 1u ? 0u : 1u;
	else
		recovered = quadrature_residue == 3u ? 2u : 3u;
	*quarter_turns = recovered;
	return true;
}

enum sippymodem_v34_mapper_result sippymodem_v34_unmap_2d_symbol(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_grid_point *mapper_output,
	struct sippymodem_v34_2d_mapping_input *input)
{
	struct sippymodem_v34_2d_mapping_input recovered;
	struct sippymodem_v34_grid_point quarter_point;
	size_t label;
	uint16_t point_mask;
	uint8_t clockwise_quarter_turns;

	if (!sippymodem_v34_mapping_parameters_is_valid(parameters) ||
	    mapper_output == NULL || input == NULL ||
	    !recover_clockwise_quarter_turns(
		    mapper_output, &clockwise_quarter_turns) ||
	    !rotate_clockwise(
		    mapper_output,
		    (uint8_t)((4u - clockwise_quarter_turns) & 3u),
		    &quarter_point) ||
	    sippymodem_v34_quarter_superconstellation_label(
		    &quarter_point, &label) != SIPPYMODEM_V34_CONSTELLATION_OK ||
	    label >= parameters->constellation_points / 4u)
		return SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT;
	point_mask = (uint16_t)(
		(1u << parameters->point_index_bits_per_2d_symbol) - 1u);
	recovered = (struct sippymodem_v34_2d_mapping_input){
		.ring_index = (uint8_t)(label >>
			parameters->point_index_bits_per_2d_symbol),
		.point_index = (uint8_t)(label & point_mask),
		.clockwise_quarter_turns = clockwise_quarter_turns,
	};
	if (!sippymodem_v34_2d_mapping_input_is_valid(&recovered, parameters))
		return SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT;
	*input = recovered;
	return SIPPYMODEM_V34_MAPPER_OK;
}

const char *sippymodem_v34_mapper_result_name(
	enum sippymodem_v34_mapper_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPER_OK:
		return "ok";
	case SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
