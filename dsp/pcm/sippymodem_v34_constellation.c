// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_constellation.h"

static const double V34_POINT_ZERO_COORDINATE =
	0.70710678118654752440084436210485;

struct sippymodem_qam_point sippymodem_v34_rotated_point_zero(
	int counterclockwise_quarter_turns)
{
	double coordinate = V34_POINT_ZERO_COORDINATE;
	int turns = counterclockwise_quarter_turns % 4;

	if (turns < 0)
		turns += 4;
	switch (turns) {
	case 0:
		return (struct sippymodem_qam_point){ coordinate, coordinate };
	case 1:
		return (struct sippymodem_qam_point){ -coordinate, coordinate };
	case 2:
		return (struct sippymodem_qam_point){ -coordinate, -coordinate };
	case 3:
		return (struct sippymodem_qam_point){ coordinate, -coordinate };
	}
	return (struct sippymodem_qam_point){ 0 };
}
