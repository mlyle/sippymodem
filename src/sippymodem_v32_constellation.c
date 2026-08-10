// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_constellation.h"

#include <math.h>
#include <stddef.h>

static double signed_coordinate(bool positive, bool outer)
{
	double magnitude = outer ? 3.0 : 1.0;

	return (positive ? magnitude : -magnitude) / sqrt(18.0);
}

enum sippymodem_v32_constellation_result
sippymodem_v32_constellation_point(
	uint8_t symbol, struct sippymodem_qam_point *point)
{
	struct sippymodem_qam_point next;
	uint8_t quadrant;
	uint8_t position;
	bool swap_magnitude_bits;
	bool outer_in_phase;
	bool outer_quadrature;

	if (symbol >= SIPPYMODEM_V32_CONSTELLATION_SYMBOLS || point == NULL)
		return SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT;
	quadrant = (uint8_t)(symbol >> 2);
	position = (uint8_t)(symbol & 3u);
	swap_magnitude_bits = (quadrant & 1u) != 0;
	outer_in_phase =
		swap_magnitude_bits ? (position & 2u) != 0 :
				      (position & 1u) != 0;
	outer_quadrature =
		swap_magnitude_bits ? (position & 1u) != 0 :
				      (position & 2u) != 0;
	next = (struct sippymodem_qam_point){
		.in_phase = signed_coordinate(
			quadrant == 1 || quadrant == 2, outer_in_phase),
		.quadrature = signed_coordinate(
			quadrant >= 2, outer_quadrature),
	};
	*point = next;
	return SIPPYMODEM_V32_CONSTELLATION_OK;
}

enum sippymodem_v32_constellation_result
sippymodem_v32_4800_constellation_point(
	uint8_t quadrant, struct sippymodem_qam_point *point)
{
	if (quadrant >= SIPPYMODEM_V32_4800_SIGNAL_POINTS)
		return SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT;
	return sippymodem_v32_constellation_point(
		(uint8_t)(quadrant * 4u + 1u), point);
}

const char *sippymodem_v32_constellation_result_name(
	enum sippymodem_v32_constellation_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_CONSTELLATION_OK:
		return "ok";
	case SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
