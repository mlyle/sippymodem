// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_mapper.h"

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

static struct sippymodem_v34_grid_point rotate_clockwise(
	struct sippymodem_v34_grid_point point, uint8_t quarter_turns)
{
	switch (quarter_turns) {
	case 0:
		return point;
	case 1:
		return (struct sippymodem_v34_grid_point){
			.in_phase = point.quadrature,
			.quadrature = (int16_t)-point.in_phase,
		};
	case 2:
		return (struct sippymodem_v34_grid_point){
			.in_phase = (int16_t)-point.in_phase,
			.quadrature = (int16_t)-point.quadrature,
		};
	case 3:
		return (struct sippymodem_v34_grid_point){
			.in_phase = (int16_t)-point.quadrature,
			.quadrature = point.in_phase,
		};
	}
	return (struct sippymodem_v34_grid_point){ 0 };
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
	generated = (struct sippymodem_v34_2d_mapping_output){
		.constellation_label = label,
		.mapper_output = rotate_clockwise(
			quarter_point, input->clockwise_quarter_turns),
	};
	*output = generated;
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
