// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_CONSTELLATION_H
#define SIPPYMODEM_V34_CONSTELLATION_H

#include "sippymodem_qam.h"

enum sippymodem_v34_constellation_size {
	SIPPYMODEM_V34_CONSTELLATION_4_POINT = 4,
	SIPPYMODEM_V34_CONSTELLATION_16_POINT = 16,
};

/* Return point zero of the V.34 quarter-superconstellation after the given
 * number of 90-degree turns. Positive turns are counterclockwise; negative
 * turns are clockwise. */
struct sippymodem_qam_point sippymodem_v34_rotated_point_zero(
	int counterclockwise_quarter_turns);

#endif
