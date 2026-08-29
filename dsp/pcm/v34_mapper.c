// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapper.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

bool v34_2d_mapping_input_is_valid(
	const struct v34_2d_mapping_input *input,
	const struct v34_mapping_parameters *parameters)
{
	uint16_t point_index_limit;

	if (input == NULL ||
	    !v34_mapping_parameters_is_valid(parameters))
		return false;
	point_index_limit = (uint16_t)(
		1u << parameters->point_index_bits_per_2d_symbol);
	return input->ring_index < parameters->ring_count &&
	       input->point_index < point_index_limit &&
	       input->clockwise_quarter_turns < 4u;
}

static bool rotate_clockwise(
	const struct v34_grid_point *point, uint8_t quarter_turns,
	struct v34_grid_point *rotated)
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
	*rotated = (struct v34_grid_point){
		.in_phase = (int16_t)in_phase,
		.quadrature = (int16_t)quadrature,
	};
	return true;
}

enum v34_mapper_result v34_map_2d_symbol(
	const struct v34_mapping_parameters *parameters,
	const struct v34_2d_mapping_input *input,
	struct v34_2d_mapping_output *output)
{
	struct v34_2d_mapping_output generated;
	struct v34_grid_point quarter_point;
	uint16_t label;

	if (!v34_2d_mapping_input_is_valid(input, parameters) ||
	    output == NULL)
		return V34_MAPPER_INVALID_ARGUMENT;
	label = (uint16_t)(
		((uint16_t)input->ring_index <<
		 parameters->point_index_bits_per_2d_symbol) |
		input->point_index);
	if (label >= parameters->constellation_points / 4u ||
	    v34_quarter_superconstellation_point(
		    label, &quarter_point) !=
		    V34_CONSTELLATION_OK)
		return V34_MAPPER_INVALID_ARGUMENT;
	generated.constellation_label = label;
	if (!rotate_clockwise(
		    &quarter_point, input->clockwise_quarter_turns,
		    &generated.mapper_output))
		return V34_MAPPER_INVALID_ARGUMENT;
	*output = generated;
	return V34_MAPPER_OK;
}

static bool recover_clockwise_quarter_turns(
	const struct v34_grid_point *point, uint8_t *quarter_turns)
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

static bool recover_label_and_rotation(
	const struct v34_grid_point *mapper_output, size_t *label,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_grid_point quarter_point;

	return recover_clockwise_quarter_turns(
		       mapper_output, clockwise_quarter_turns) &&
	       rotate_clockwise(
		       mapper_output,
		       (uint8_t)((4u - *clockwise_quarter_turns) & 3u),
		       &quarter_point) &&
	       v34_quarter_superconstellation_label(
		       &quarter_point, label) == V34_CONSTELLATION_OK;
}

static bool grid_point_is_bounded(const struct v34_grid_point *point)
{
	return point->in_phase >= -V34_CHANNEL_COORDINATE_LIMIT &&
	       point->in_phase <= V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature >= -V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature <= V34_CHANNEL_COORDINATE_LIMIT;
}

static bool recover_nearest_label_and_rotation(
	const struct v34_grid_point *mapper_output, size_t *label,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_grid_point points[
		V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	struct v34_grid_point quarter_point;
	int64_t best_distance = INT64_MAX;
	size_t best_label = 0;

	if (mapper_output == NULL || label == NULL ||
	    clockwise_quarter_turns == NULL ||
	    !grid_point_is_bounded(mapper_output) ||
	    !recover_clockwise_quarter_turns(
		    mapper_output, clockwise_quarter_turns) ||
	    !rotate_clockwise(
		    mapper_output,
		    (uint8_t)((4u - *clockwise_quarter_turns) & 3u),
		    &quarter_point) ||
	    v34_quarter_superconstellation_generate(
		    points, V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT) !=
		    V34_CONSTELLATION_OK)
		return false;
	for (size_t candidate = 0;
	     candidate < V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT;
	     ++candidate) {
		int64_t in_phase_error =
			(int64_t)quarter_point.in_phase - points[candidate].in_phase;
		int64_t quadrature_error =
			(int64_t)quarter_point.quadrature -
			points[candidate].quadrature;
		int64_t distance = in_phase_error * in_phase_error +
			quadrature_error * quadrature_error;

		if (distance < best_distance) {
			best_distance = distance;
			best_label = candidate;
		}
	}
	*label = best_label;
	return true;
}

static struct v34_2d_mapping_input recovered_input(
	const struct v34_mapping_parameters *parameters, size_t label,
	uint8_t clockwise_quarter_turns)
{
	uint16_t point_mask = (uint16_t)(
		(1u << parameters->point_index_bits_per_2d_symbol) - 1u);

	return (struct v34_2d_mapping_input){
		.ring_index = (uint8_t)(label >>
			parameters->point_index_bits_per_2d_symbol),
		.point_index = (uint8_t)(label & point_mask),
		.clockwise_quarter_turns = clockwise_quarter_turns,
	};
}

static struct v34_2d_mapping_decision recovered_decision(
	const struct v34_mapping_parameters *parameters, size_t label,
	uint8_t clockwise_quarter_turns)
{
	uint16_t point_mask = (uint16_t)(
		(1u << parameters->point_index_bits_per_2d_symbol) - 1u);

	return (struct v34_2d_mapping_decision){
		.ring_prefix = (uint16_t)(label >>
			parameters->point_index_bits_per_2d_symbol),
		.point_index = (uint8_t)(label & point_mask),
		.clockwise_quarter_turns = clockwise_quarter_turns,
	};
}

enum v34_mapper_result v34_unmap_2d_symbol(
	const struct v34_mapping_parameters *parameters,
	const struct v34_grid_point *mapper_output,
	struct v34_2d_mapping_input *input)
{
	struct v34_2d_mapping_input recovered;
	size_t label;
	uint8_t clockwise_quarter_turns;

	if (!v34_mapping_parameters_is_valid(parameters) ||
	    mapper_output == NULL || input == NULL ||
	    !recover_label_and_rotation(
		    mapper_output, &label, &clockwise_quarter_turns) ||
	    label >= parameters->constellation_points / 4u)
		return V34_MAPPER_INVALID_ARGUMENT;
	recovered = recovered_input(
		parameters, label, clockwise_quarter_turns);
	if (!v34_2d_mapping_input_is_valid(&recovered, parameters))
		return V34_MAPPER_INVALID_ARGUMENT;
	*input = recovered;
	return V34_MAPPER_OK;
}

enum v34_mapper_result v34_unmap_2d_decision(
	const struct v34_mapping_parameters *parameters,
	const struct v34_grid_point *mapper_output,
	struct v34_2d_mapping_decision *decision)
{
	struct v34_2d_mapping_decision recovered;
	size_t label;
	uint8_t clockwise_quarter_turns;

	if (!v34_mapping_parameters_is_valid(parameters) ||
	    mapper_output == NULL || decision == NULL ||
	    (!recover_label_and_rotation(
		     mapper_output, &label, &clockwise_quarter_turns) &&
	     !recover_nearest_label_and_rotation(
		     mapper_output, &label, &clockwise_quarter_turns)))
		return V34_MAPPER_INVALID_ARGUMENT;
	recovered = recovered_decision(
		parameters, label, clockwise_quarter_turns);
	*decision = recovered;
	return V34_MAPPER_OK;
}
