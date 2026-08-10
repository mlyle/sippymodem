// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_CONSTELLATION_H
#define SIPPYMODEM_V34_CONSTELLATION_H

#include "sippymodem_qam.h"

#include <stdbool.h>

enum sippymodem_v34_constellation_size {
	SIPPYMODEM_V34_CONSTELLATION_4_POINT = 4,
	SIPPYMODEM_V34_CONSTELLATION_16_POINT = 16,
};

enum sippymodem_v34_constellation_result {
	SIPPYMODEM_V34_CONSTELLATION_OK,
	SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT,
};

bool sippymodem_v34_constellation_size_is_valid(
	enum sippymodem_v34_constellation_size constellation);

/* Return point zero of the V.34 quarter-superconstellation after the given
 * number of 90-degree turns. Positive turns are counterclockwise; negative
 * turns are clockwise. */
struct sippymodem_qam_point sippymodem_v34_rotated_point_zero(
	int counterclockwise_quarter_turns);

/* Generate a unit-average-power Phase 3/4 training point. For the four-point
 * constellation, quarter_point must be zero. For the sixteen-point
 * constellation, quarter_point selects Figure 5 points 0 through 3. */
enum sippymodem_v34_constellation_result
sippymodem_v34_training_constellation_point(
	enum sippymodem_v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct sippymodem_qam_point *point);

const char *sippymodem_v34_constellation_result_name(
	enum sippymodem_v34_constellation_result result);

#endif
