// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CONSTELLATION_H
#define V34_CONSTELLATION_H

#include "qam.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT = 416,
	V34_CHANNEL_COORDINATE_LIMIT = 255,
	V34_2D_SYMBOLS_PER_4D_INTERVAL = 2,
};

struct v34_grid_point {
	int16_t in_phase;
	int16_t quadrature;
};

enum v34_constellation_size {
	V34_CONSTELLATION_4_POINT = 4,
	V34_CONSTELLATION_16_POINT = 16,
};

enum v34_constellation_result {
	V34_CONSTELLATION_OK,
	V34_CONSTELLATION_INVALID_ARGUMENT,
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

const char *v34_constellation_result_name(
	enum v34_constellation_result result);

#endif
