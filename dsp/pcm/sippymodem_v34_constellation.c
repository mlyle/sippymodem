// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_constellation.h"

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

bool sippymodem_v34_constellation_size_is_valid(
	enum sippymodem_v34_constellation_size constellation)
{
	return constellation == SIPPYMODEM_V34_CONSTELLATION_4_POINT ||
	       constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT;
}

static int squared_magnitude(int in_phase, int quadrature)
{
	return in_phase * in_phase + quadrature * quadrature;
}

enum sippymodem_v34_constellation_result
sippymodem_v34_quarter_superconstellation_generate(
	struct sippymodem_v34_grid_point *points, size_t point_count)
{
	struct sippymodem_v34_grid_point
		generated[SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	size_t generated_count = 0;

	if (points == NULL || point_count == 0 ||
	    point_count >
		    SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;

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
					(struct sippymodem_v34_grid_point){
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
		return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
	for (size_t index = 0; index < point_count; ++index)
		points[index] = generated[index];
	return SIPPYMODEM_V34_CONSTELLATION_OK;
}

static size_t magnitude_bucket(int in_phase, int quadrature)
{
	return (size_t)(
		(squared_magnitude(in_phase, quadrature) -
		 V34_QUARTER_FIRST_SQUARED_MAGNITUDE) /
		V34_QUARTER_SQUARED_MAGNITUDE_STEP);
}

enum sippymodem_v34_constellation_result
sippymodem_v34_quarter_superconstellation_point(
	size_t label, struct sippymodem_v34_grid_point *point)
{
	uint16_t magnitude_counts[V34_QUARTER_MAGNITUDE_BUCKET_COUNT] = { 0 };
	size_t rank = label;
	size_t selected_bucket = V34_QUARTER_MAGNITUDE_BUCKET_COUNT;
	int selected_magnitude;

	if (point == NULL ||
	    label >= SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT)
		return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
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
		return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
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
			*point = (struct sippymodem_v34_grid_point){
				.in_phase = (int16_t)in_phase,
				.quadrature = (int16_t)quadrature,
			};
			return SIPPYMODEM_V34_CONSTELLATION_OK;
		}
	}
	return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
}

static struct sippymodem_qam_point rotate_point(
	struct sippymodem_qam_point point, int counterclockwise_quarter_turns)
{
	int turns = counterclockwise_quarter_turns % 4;

	if (turns < 0)
		turns += 4;
	switch (turns) {
	case 0:
		return point;
	case 1:
		return (struct sippymodem_qam_point){
			-point.quadrature,
			point.in_phase,
		};
	case 2:
		return (struct sippymodem_qam_point){
			-point.in_phase,
			-point.quadrature,
		};
	case 3:
		return (struct sippymodem_qam_point){
			point.quadrature,
			-point.in_phase,
		};
	}
	return (struct sippymodem_qam_point){ 0 };
}

struct sippymodem_qam_point sippymodem_v34_rotated_point_zero(
	int counterclockwise_quarter_turns)
{
	struct sippymodem_qam_point point = {
		V34_POINT_ZERO_COORDINATE,
		V34_POINT_ZERO_COORDINATE,
	};

	return rotate_point(point, counterclockwise_quarter_turns);
}

enum sippymodem_v34_constellation_result
sippymodem_v34_training_constellation_point(
	enum sippymodem_v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct sippymodem_qam_point *point)
{
	struct sippymodem_qam_point next;
	double grid_unit;

	if (point == NULL || clockwise_quarter_turns > 3u ||
	    !sippymodem_v34_constellation_size_is_valid(constellation))
		return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
	switch (constellation) {
	case SIPPYMODEM_V34_CONSTELLATION_4_POINT:
		if (quarter_point != 0u)
			return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
		grid_unit = V34_POINT_ZERO_COORDINATE;
		break;
	case SIPPYMODEM_V34_CONSTELLATION_16_POINT:
		if (quarter_point > 3u)
			return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
		grid_unit = V34_SIXTEEN_POINT_GRID_UNIT;
		break;
	default:
		return SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT;
	}
	next = (struct sippymodem_qam_point){
		.in_phase = (quarter_point & 1u) != 0u ? -3.0 * grid_unit :
							       grid_unit,
		.quadrature = (quarter_point & 2u) != 0u ? -3.0 * grid_unit :
								grid_unit,
	};
	next = rotate_point(next, -(int)clockwise_quarter_turns);
	*point = next;
	return SIPPYMODEM_V34_CONSTELLATION_OK;
}

const char *sippymodem_v34_constellation_result_name(
	enum sippymodem_v34_constellation_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CONSTELLATION_OK:
		return "ok";
	case SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
