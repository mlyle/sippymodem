// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_constellation.h"

#include <stddef.h>

static const double V34_POINT_ZERO_COORDINATE =
	0.70710678118654752440084436210485;
static const double V34_SIXTEEN_POINT_GRID_UNIT =
	0.31622776601683793319988935444327;

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

	if (point == NULL || clockwise_quarter_turns > 3u)
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
