// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CONSTELLATION_H
#define V34_CONSTELLATION_H

#include "qam.h"
#include "v34_analytic_filter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT = 416,
	V34_CHANNEL_COORDINATE_LIMIT = 255,
	V34_2D_SYMBOLS_PER_4D_INTERVAL = 2,
	/* Unit-power Phase 3/4 points occupy this fixed receive radius. */
	V34_TRAINING_FIXED_AMPLITUDE = 3238,
};

struct v34_grid_point {
	int16_t in_phase;
	int16_t quadrature;
};

static inline bool v34_grid_point_is_bounded(
	const struct v34_grid_point *point)
{
	return point != NULL &&
	       point->in_phase >= -V34_CHANNEL_COORDINATE_LIMIT &&
	       point->in_phase <= V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature >= -V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature <= V34_CHANNEL_COORDINATE_LIMIT;
}

static inline bool v34_grid_point_coordinates_are_odd(
	const struct v34_grid_point *point)
{
	return point != NULL && ((uint16_t)point->in_phase & 1u) != 0u &&
	       ((uint16_t)point->quadrature & 1u) != 0u;
}

static inline bool v34_grid_point_coordinates_are_even(
	const struct v34_grid_point *point)
{
	return point != NULL && ((uint16_t)point->in_phase & 1u) == 0u &&
	       ((uint16_t)point->quadrature & 1u) == 0u;
}

enum v34_constellation_size {
	V34_CONSTELLATION_4_POINT = 4,
	V34_CONSTELLATION_16_POINT = 16,
};

/* Return zero for a value outside the Phase 3/4 training constellations. */
static inline unsigned int v34_constellation_bits_per_symbol(
	enum v34_constellation_size constellation)
{
	return constellation == V34_CONSTELLATION_4_POINT ? 2u :
	       constellation == V34_CONSTELLATION_16_POINT ? 4u : 0u;
}

/* Phase 3/4 labels shared by the point generators and control receivers. */
static inline bool v34_training_decision_is_valid(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	return clockwise_quarter_turns < 4u &&
	       ((constellation == V34_CONSTELLATION_4_POINT &&
		 quarter_point == 0u) ||
		(constellation == V34_CONSTELLATION_16_POINT &&
		 quarter_point < 4u));
}

enum v34_constellation_result {
	V34_CONSTELLATION_OK,
	V34_CONSTELLATION_INVALID_ARGUMENT,
};

struct v34_fixed_training_decision {
	struct v34_analytic_sample target;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
};

bool v34_constellation_size_is_valid(
	enum v34_constellation_size constellation);

/* Generate labels zero through point_count - 1 from the V.34 quarter-
 * superconstellation. Coordinates are exact, unscaled odd grid integers.
 * The destination is unchanged when an argument is rejected. */
enum v34_constellation_result
v34_quarter_superconstellation_generate(
	struct v34_grid_point *points, size_t point_count);

/* Generate one Figure 5 point by label without retaining the constellation as
 * a lookup table. The destination is unchanged for an invalid label. */
enum v34_constellation_result
v34_quarter_superconstellation_point(
	size_t label, struct v34_grid_point *point);

/* Recover the exact Figure 5 label for an unrotated quarter-
 * superconstellation point. No inverse lookup table is retained, and the
 * destination is unchanged when the point is outside the defined prefix. */
enum v34_constellation_result
v34_quarter_superconstellation_label(
	const struct v34_grid_point *point, size_t *label);

/* Return point zero of the V.34 quarter-superconstellation after the given
 * number of 90-degree turns. Positive turns are counterclockwise; negative
 * turns are clockwise. */
struct qam_point v34_rotated_point_zero(
	int counterclockwise_quarter_turns);

/* Generate a unit-average-power Phase 3/4 training point. For the four-point
 * constellation, quarter_point must be zero. For the sixteen-point
 * constellation, quarter_point selects Figure 5 points 0 through 3. */
enum v34_constellation_result
v34_training_constellation_point(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct qam_point *point);

/* Select the nearest generated Phase 3/4 training point. Equal-distance ties
 * choose the first label in rotation-major order. Rejected calls leave both
 * outputs unchanged. */
enum v34_constellation_result
v34_training_constellation_slice(
	enum v34_constellation_size constellation,
	const struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

/* Generate and slice the same Phase 3/4 constellation in the fixed receive
 * coordinates used by the persistent V.34 equalizer.  The four-point set
 * preserves its asymmetric positive and negative half-scale endpoints. */
enum v34_constellation_result
v34_training_constellation_fixed_point(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v34_analytic_sample *point);

/* Equal-distance ties use the same rotation-major, quarter-point-minor order
 * as the normalized slicer.  Rejected calls leave the decision unchanged. */
enum v34_constellation_result
v34_training_constellation_fixed_slice(
	enum v34_constellation_size constellation,
	struct v34_analytic_sample point,
	struct v34_fixed_training_decision *decision);

/* Qualify decision-directed training feedback at half the innermost point's
 * radius. This rejects near-silence without excluding the sixteen-point
 * set's inner points. Not a carrier detector or a mapped-data qualifier.
 * Invalid constellations return false. */
bool v34_training_constellation_has_signal(
	enum v34_constellation_size constellation,
	struct v34_analytic_sample point);

#endif
