// SPDX-License-Identifier: BSD-3-Clause

#include "v34_constellation.h"
#include "v34_constellation_points.h"
#include "sample_math.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(V34_QUARTER_POINTS) / sizeof(V34_QUARTER_POINTS[0]) ==
	V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT, "generated constellation size");

static const double V34_POINT_ZERO_COORDINATE =
	0.70710678118654752440084436210485;
static const double V34_SIXTEEN_POINT_GRID_UNIT =
	0.31622776601683793319988935444327;

bool v34_constellation_size_is_valid(
	enum v34_constellation_size constellation)
{
	return v34_constellation_bits_per_symbol(constellation) != 0u;
}

enum v34_constellation_result
v34_quarter_superconstellation_generate(
	struct v34_grid_point *points, size_t point_count)
{
	if (points == NULL || point_count == 0 ||
	    point_count > V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	memcpy(points, V34_QUARTER_POINTS, point_count * sizeof(*points));
	return V34_CONSTELLATION_OK;
}

enum v34_constellation_result
v34_quarter_superconstellation_point(
	size_t label, struct v34_grid_point *point)
{
	if (point == NULL || label >= V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	*point = V34_QUARTER_POINTS[label];
	return V34_CONSTELLATION_OK;
}

static bool quarter_grid_coordinate_is_valid(int16_t coordinate)
{
	return coordinate >= V34_QUARTER_GRID_MIN &&
	       coordinate <= V34_QUARTER_GRID_MAX &&
	       ((uint16_t)coordinate & 3u) == 1u;
}

enum v34_constellation_result
v34_quarter_superconstellation_label(
	const struct v34_grid_point *point, size_t *label)
{
	if (point == NULL || label == NULL ||
	    !quarter_grid_coordinate_is_valid(point->in_phase) ||
	    !quarter_grid_coordinate_is_valid(point->quadrature))
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	uint16_t selected = V34_QUARTER_LABELS
		[(point->quadrature - V34_QUARTER_GRID_MIN) / V34_QUARTER_GRID_STEP]
		[(point->in_phase - V34_QUARTER_GRID_MIN) / V34_QUARTER_GRID_STEP];
	if (selected >= V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	*label = selected;
	return V34_CONSTELLATION_OK;
}

struct qam_point v34_rotated_point_zero(
	int counterclockwise_quarter_turns)
{
	struct qam_point point = {
		V34_POINT_ZERO_COORDINATE,
		V34_POINT_ZERO_COORDINATE,
	};

	return qam_point_rotate_quarters(point, counterclockwise_quarter_turns);
}

static void rotate_clockwise_grid(
	int *in_phase, int *quadrature, uint8_t turns)
{
	int saved;

	switch (turns) {
	case 0:
		break;
	case 1:
		saved = *in_phase;
		*in_phase = *quadrature;
		*quadrature = -saved;
		break;
	case 2:
		*in_phase = -*in_phase;
		*quadrature = -*quadrature;
		break;
	case 3:
		saved = *in_phase;
		*in_phase = -*quadrature;
		*quadrature = saved;
		break;
	}
}

static bool training_grid_coordinates(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	int *in_phase, int *quadrature)
{
	if (!v34_training_decision_is_valid(
		    constellation, quarter_point, clockwise_quarter_turns))
		return false;
	*in_phase = (quarter_point & 1u) != 0u ? -3 : 1;
	*quadrature = (quarter_point & 2u) != 0u ? -3 : 1;
	rotate_clockwise_grid(
		in_phase, quadrature, clockwise_quarter_turns);
	return true;
}

enum v34_constellation_result
v34_training_constellation_point(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct qam_point *point)
{
	double grid_unit;
	int in_phase;
	int quadrature;

	if (point == NULL || !training_grid_coordinates(
		    constellation, quarter_point, clockwise_quarter_turns,
		    &in_phase, &quadrature))
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	switch (constellation) {
	case V34_CONSTELLATION_4_POINT:
		grid_unit = V34_POINT_ZERO_COORDINATE;
		break;
	case V34_CONSTELLATION_16_POINT:
		grid_unit = V34_SIXTEEN_POINT_GRID_UNIT;
		break;
	default:
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	}
	*point = (struct qam_point){
		.in_phase = in_phase * grid_unit,
		.quadrature = quadrature * grid_unit,
	};
	return V34_CONSTELLATION_OK;
}

enum v34_constellation_result
v34_training_constellation_slice(
	enum v34_constellation_size constellation,
	const struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	double best_distance = DBL_MAX;
	uint8_t best_quarter = 0;
	uint8_t best_rotation = 0;
	uint8_t quarter_count;
	bool found = false;

	if (!v34_constellation_size_is_valid(constellation) ||
	    !point_is_finite(point) || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    quarter_point == clockwise_quarter_turns)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	quarter_count = constellation == V34_CONSTELLATION_16_POINT ?
				4u :
				1u;
	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		for (uint8_t quarter = 0; quarter < quarter_count; ++quarter) {
			struct qam_point candidate;
			double distance;

			if (v34_training_constellation_point(
				    constellation, quarter, rotation,
				    &candidate) != V34_CONSTELLATION_OK)
				return V34_CONSTELLATION_INVALID_ARGUMENT;
			distance = qam_point_distance_squared(point, &candidate);
			if (!isfinite(distance) || distance >= best_distance)
				continue;
			best_distance = distance;
			best_quarter = quarter;
			best_rotation = rotation;
			found = true;
		}
	}
	if (!found)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	*quarter_point = best_quarter;
	*clockwise_quarter_turns = best_rotation;
	return V34_CONSTELLATION_OK;
}

enum v34_constellation_result
v34_training_constellation_fixed_point(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v34_analytic_sample *point)
{
	struct v34_analytic_sample generated;
	int in_phase;
	int quadrature;

	if (point == NULL || !training_grid_coordinates(
		    constellation, quarter_point, clockwise_quarter_turns,
		    &in_phase, &quadrature))
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	if (constellation == V34_CONSTELLATION_4_POINT) {
		generated.in_phase = in_phase > 0 ? 2289 : -2290;
		generated.quadrature = quadrature > 0 ? 2289 : -2290;
	} else {
		generated.in_phase = (int16_t)(in_phase * 1024);
		generated.quadrature = (int16_t)(quadrature * 1024);
	}
	*point = generated;
	return V34_CONSTELLATION_OK;
}

bool v34_training_constellation_has_signal(
	enum v34_constellation_size constellation,
	struct v34_analytic_sample point)
{
	struct v34_analytic_sample innermost;

	if (v34_training_constellation_fixed_point(
		    constellation, 0, 0, &innermost) != V34_CONSTELLATION_OK)
		return false;
	return sample_pair_power_s16(point.in_phase, point.quadrature) >=
	       sample_pair_power_s16(innermost.in_phase, innermost.quadrature) / 4u;
}

enum v34_constellation_result
v34_training_constellation_fixed_slice(
	enum v34_constellation_size constellation,
	struct v34_analytic_sample point,
	struct v34_fixed_training_decision *decision)
{
	struct v34_fixed_training_decision generated = { 0 };
	uint64_t best_distance = UINT64_MAX;
	uint8_t quarter_count;
	bool found = false;

	if (!v34_constellation_size_is_valid(constellation) ||
	    decision == NULL)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	quarter_count = constellation == V34_CONSTELLATION_16_POINT ? 4u : 1u;
	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		for (uint8_t quarter = 0; quarter < quarter_count; ++quarter) {
			struct v34_analytic_sample target;
			uint64_t distance;

			if (v34_training_constellation_fixed_point(
				    constellation, quarter, rotation, &target) !=
			    V34_CONSTELLATION_OK)
				return V34_CONSTELLATION_INVALID_ARGUMENT;
			distance = sample_pair_distance_s16(point.in_phase, point.quadrature,
				target.in_phase, target.quadrature);
			if (distance >= best_distance)
				continue;
			best_distance = distance;
			generated = (struct v34_fixed_training_decision){
				.target = target,
				.quarter_point = quarter,
				.clockwise_quarter_turns = rotation,
			};
			found = true;
		}
	}
	if (!found)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	*decision = generated;
	return V34_CONSTELLATION_OK;
}
