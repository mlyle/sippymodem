// SPDX-License-Identifier: BSD-3-Clause

#include "v34_constellation.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

enum {
	/* Figure 5 uses the lattice coordinate 1 modulo 4 on both axes. The
	 * first 416 points fit in this square; the next lattice coordinate
	 * outside it has greater magnitude than point 415. */
	V34_QUARTER_GRID_MIN = -43,
	V34_QUARTER_GRID_MAX = 45,
	V34_QUARTER_GRID_STEP = 4,
	V34_QUARTER_FIRST_SQUARED_MAGNITUDE = 2,
	V34_QUARTER_SQUARED_MAGNITUDE_STEP = 8,
	V34_QUARTER_GRID_MAX_SQUARED_MAGNITUDE =
		2 * V34_QUARTER_GRID_MAX * V34_QUARTER_GRID_MAX,
	V34_QUARTER_MAGNITUDE_BUCKET_COUNT =
		(V34_QUARTER_GRID_MAX_SQUARED_MAGNITUDE -
		 V34_QUARTER_FIRST_SQUARED_MAGNITUDE) /
			V34_QUARTER_SQUARED_MAGNITUDE_STEP +
		1,
};

static const double V34_POINT_ZERO_COORDINATE =
	0.70710678118654752440084436210485;
static const double V34_SIXTEEN_POINT_GRID_UNIT =
	0.31622776601683793319988935444327;

bool v34_constellation_size_is_valid(
	enum v34_constellation_size constellation)
{
	return v34_constellation_bits_per_symbol(constellation) != 0u;
}

static int squared_magnitude(int in_phase, int quadrature)
{
	return in_phase * in_phase + quadrature * quadrature;
}

enum v34_constellation_result
v34_quarter_superconstellation_generate(
	struct v34_grid_point *points, size_t point_count)
{
	struct v34_grid_point
		generated[V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	size_t generated_count = 0;

	if (points == NULL || point_count == 0 ||
	    point_count >
		    V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;

	/* Odd-coordinate squared magnitudes are 2 modulo 8. Scanning only
	 * those magnitudes and visiting quadrature coordinates from greatest
	 * to least implements the normative label ordering directly. */
	for (int magnitude = V34_QUARTER_FIRST_SQUARED_MAGNITUDE;
	     magnitude <= V34_QUARTER_GRID_MAX_SQUARED_MAGNITUDE &&
	     generated_count < point_count;
	     magnitude += V34_QUARTER_SQUARED_MAGNITUDE_STEP) {
		for (int quadrature = V34_QUARTER_GRID_MAX;
		     quadrature >= V34_QUARTER_GRID_MIN;
		     quadrature -= V34_QUARTER_GRID_STEP) {
			for (int in_phase = V34_QUARTER_GRID_MIN;
			     in_phase <= V34_QUARTER_GRID_MAX;
			     in_phase += V34_QUARTER_GRID_STEP) {
				if (squared_magnitude(in_phase, quadrature) !=
				    magnitude)
					continue;
				generated[generated_count++] =
					(struct v34_grid_point){
						.in_phase = (int16_t)in_phase,
						.quadrature = (int16_t)quadrature,
					};
				if (generated_count == point_count)
					break;
			}
			if (generated_count == point_count)
				break;
		}
	}
	if (generated_count != point_count)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	for (size_t index = 0; index < point_count; ++index)
		points[index] = generated[index];
	return V34_CONSTELLATION_OK;
}

static size_t magnitude_bucket(int in_phase, int quadrature)
{
	return (size_t)(
		(squared_magnitude(in_phase, quadrature) -
		 V34_QUARTER_FIRST_SQUARED_MAGNITUDE) /
		V34_QUARTER_SQUARED_MAGNITUDE_STEP);
}

enum v34_constellation_result
v34_quarter_superconstellation_point(
	size_t label, struct v34_grid_point *point)
{
	uint16_t magnitude_counts[V34_QUARTER_MAGNITUDE_BUCKET_COUNT] = { 0 };
	size_t rank = label;
	size_t selected_bucket = V34_QUARTER_MAGNITUDE_BUCKET_COUNT;
	int selected_magnitude;

	if (point == NULL ||
	    label >= V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	/* A generated histogram finds the label's magnitude in one lattice
	 * pass, avoiding both a retained table and the bulk generator's scan
	 * across every empty squared magnitude. */
	for (int quadrature = V34_QUARTER_GRID_MAX;
	     quadrature >= V34_QUARTER_GRID_MIN;
	     quadrature -= V34_QUARTER_GRID_STEP) {
		for (int in_phase = V34_QUARTER_GRID_MIN;
		     in_phase <= V34_QUARTER_GRID_MAX;
		     in_phase += V34_QUARTER_GRID_STEP)
			magnitude_counts[magnitude_bucket(in_phase, quadrature)]++;
	}
	for (size_t bucket = 0;
	     bucket < V34_QUARTER_MAGNITUDE_BUCKET_COUNT; ++bucket) {
		if (rank < magnitude_counts[bucket]) {
			selected_bucket = bucket;
			break;
		}
		rank -= magnitude_counts[bucket];
	}
	if (selected_bucket == V34_QUARTER_MAGNITUDE_BUCKET_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	selected_magnitude = V34_QUARTER_FIRST_SQUARED_MAGNITUDE +
		(int)selected_bucket * V34_QUARTER_SQUARED_MAGNITUDE_STEP;
	for (int quadrature = V34_QUARTER_GRID_MAX;
	     quadrature >= V34_QUARTER_GRID_MIN;
	     quadrature -= V34_QUARTER_GRID_STEP) {
		for (int in_phase = V34_QUARTER_GRID_MIN;
		     in_phase <= V34_QUARTER_GRID_MAX;
		     in_phase += V34_QUARTER_GRID_STEP) {
			if (squared_magnitude(in_phase, quadrature) !=
			    selected_magnitude)
				continue;
			if (rank != 0) {
				--rank;
				continue;
			}
			*point = (struct v34_grid_point){
				.in_phase = (int16_t)in_phase,
				.quadrature = (int16_t)quadrature,
			};
			return V34_CONSTELLATION_OK;
		}
	}
	return V34_CONSTELLATION_INVALID_ARGUMENT;
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
	size_t rank = 0;
	int selected_magnitude;

	if (point == NULL || label == NULL ||
	    !quarter_grid_coordinate_is_valid(point->in_phase) ||
	    !quarter_grid_coordinate_is_valid(point->quadrature))
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	selected_magnitude = squared_magnitude(
		point->in_phase, point->quadrature);
	/* Labels are ordered first by magnitude, then by descending
	 * quadrature and ascending in-phase coordinate. Counting preceding
	 * lattice points reconstructs that order without the 416-word table
	 * traditionally used by a demapper. */
	for (int quadrature = V34_QUARTER_GRID_MAX;
	     quadrature >= V34_QUARTER_GRID_MIN;
	     quadrature -= V34_QUARTER_GRID_STEP) {
		for (int in_phase = V34_QUARTER_GRID_MIN;
		     in_phase <= V34_QUARTER_GRID_MAX;
		     in_phase += V34_QUARTER_GRID_STEP) {
			int magnitude = squared_magnitude(in_phase, quadrature);

			if (magnitude < selected_magnitude ||
			    (magnitude == selected_magnitude &&
			     (quadrature > point->quadrature ||
			      (quadrature == point->quadrature &&
			       in_phase < point->in_phase))))
				rank++;
		}
	}
	if (rank >= V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	*label = rank;
	return V34_CONSTELLATION_OK;
}

static struct qam_point rotate_point(
	struct qam_point point, int counterclockwise_quarter_turns)
{
	int turns = counterclockwise_quarter_turns % 4;

	if (turns < 0)
		turns += 4;
	switch (turns) {
	case 0:
		return point;
	case 1:
		return (struct qam_point){
			-point.quadrature,
			point.in_phase,
		};
	case 2:
		return (struct qam_point){
			-point.in_phase,
			-point.quadrature,
		};
	case 3:
		return (struct qam_point){
			point.quadrature,
			-point.in_phase,
		};
	}
	return (struct qam_point){ 0 };
}

struct qam_point v34_rotated_point_zero(
	int counterclockwise_quarter_turns)
{
	struct qam_point point = {
		V34_POINT_ZERO_COORDINATE,
		V34_POINT_ZERO_COORDINATE,
	};

	return rotate_point(point, counterclockwise_quarter_turns);
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
	if (clockwise_quarter_turns > 3u ||
	    !v34_constellation_size_is_valid(constellation) ||
	    (constellation == V34_CONSTELLATION_4_POINT ?
		     quarter_point != 0u : quarter_point > 3u))
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
	    point == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature) || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    quarter_point == clockwise_quarter_turns)
		return V34_CONSTELLATION_INVALID_ARGUMENT;
	quarter_count = constellation == V34_CONSTELLATION_16_POINT ?
				4u :
				1u;
	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		for (uint8_t quarter = 0; quarter < quarter_count; ++quarter) {
			struct qam_point candidate;
			double in_phase_error;
			double quadrature_error;
			double distance;

			if (v34_training_constellation_point(
				    constellation, quarter, rotation,
				    &candidate) != V34_CONSTELLATION_OK)
				return V34_CONSTELLATION_INVALID_ARGUMENT;
			in_phase_error = point->in_phase - candidate.in_phase;
			quadrature_error =
				point->quadrature - candidate.quadrature;
			distance = in_phase_error * in_phase_error +
				   quadrature_error * quadrature_error;
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
			int32_t in_phase_error;
			int32_t quadrature_error;
			uint64_t distance;

			if (v34_training_constellation_fixed_point(
				    constellation, quarter, rotation, &target) !=
			    V34_CONSTELLATION_OK)
				return V34_CONSTELLATION_INVALID_ARGUMENT;
			in_phase_error =
				(int32_t)point.in_phase - target.in_phase;
			quadrature_error =
				(int32_t)point.quadrature - target.quadrature;
			distance = (uint64_t)(
				(int64_t)in_phase_error * in_phase_error) +
				(uint64_t)(
					(int64_t)quadrature_error * quadrature_error);
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
