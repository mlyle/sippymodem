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

struct doubled_grid_point {
	int in_phase;
	int quadrature;
};

static bool trellis_data_bits_are_valid(uint8_t data_bits_per_symbol)
{
	return data_bits_per_symbol >= 3 && data_bits_per_symbol <= 6;
}

/*
 * The high three signal-label bits select an Ungerboeck partition subset.
 * Subsets 0/1/6/7 and 2/3/4/5 each share one seed family; sign changes and
 * quarter turns generate the other six subsets.  Coordinates are doubled so
 * that the half-grid used at the two higher rates remains exact without a
 * checked-in point table.
 */
static struct doubled_grid_point trellis_seed_point(
	uint8_t data_bits_per_symbol, bool partition_family_two,
	uint8_t low_bits)
{
	int bit_zero = low_bits & 1u;
	int bit_one = (low_bits >> 1) & 1u;
	int bit_two = (low_bits >> 2) & 1u;
	int bit_three = (low_bits >> 3) & 1u;
	struct doubled_grid_point point;

	switch (data_bits_per_symbol) {
	case 3:
		point.in_phase = 3 - 4 * bit_zero;
		point.quadrature = partition_family_two ? 1 - 4 * bit_zero :
							     -3 + 4 * bit_zero;
		break;
	case 4:
		if (partition_family_two) {
			point.in_phase = -2 + 4 * bit_one;
			point.quadrature = 3 - 4 * bit_zero;
		} else {
			point.in_phase = -4 + 4 * bit_zero + 4 * bit_one;
			point.quadrature =
				1 - 4 * bit_zero * (1 - bit_one);
		}
		break;
	case 5: {
		int corner = bit_zero * bit_one * (1 - bit_two);

		if (partition_family_two) {
			point.in_phase = -1 + 4 * bit_two + 8 * bit_one -
					 4 * bit_two * bit_one - 4 * bit_zero -
					 8 * corner;
			point.quadrature = 5 - 4 * bit_two -
					   4 * bit_two * bit_one - 4 * bit_zero -
					   8 * corner;
		} else {
			point.in_phase = 7 - 4 * bit_two -
					 4 * bit_two * bit_one - 4 * bit_zero -
					 8 * corner;
			point.quadrature = 1 - 4 * bit_two - 8 * bit_one +
					   4 * bit_two * bit_one + 4 * bit_zero +
					   8 * corner;
		}
		break;
	}
	case 6: {
		int corner = bit_zero * (bit_one | bit_two);

		if (partition_family_two) {
			point.in_phase =
				(1 - 2 * bit_three) * (2 + 4 * corner);
			point.quadrature = -1 + 4 * (bit_one - bit_two);
			if ((bit_one | bit_two) == 0)
				point.quadrature += 16 * bit_zero - 8;
		} else {
			point.in_phase = 4 * (bit_one - bit_two);
			if ((bit_one | bit_two) == 0)
				point.in_phase += 8 * (2 * bit_zero - 1);
			point.quadrature = -3 + 4 * bit_three +
				4 * (2 * bit_three - 1) * corner;
		}
		break;
	}
	default:
		return (struct doubled_grid_point){ 0 };
	}

	/* The two lower-rate sets lie on the integral rather than half grid. */
	if (data_bits_per_symbol <= 4) {
		point.in_phase *= 2;
		point.quadrature *= 2;
	}
	return point;
}

static struct doubled_grid_point trellis_subset_transform(
	uint8_t subset, struct doubled_grid_point point)
{
	struct doubled_grid_point transformed;

	switch (subset) {
	case 0:
	case 2:
		return point;
	case 1:
	case 3:
		return (struct doubled_grid_point){
			.in_phase = -point.in_phase,
			.quadrature = -point.quadrature,
		};
	case 4:
	case 7:
		transformed.in_phase = -point.quadrature;
		transformed.quadrature = point.in_phase;
		return transformed;
	case 5:
	case 6:
		transformed.in_phase = point.quadrature;
		transformed.quadrature = -point.in_phase;
		return transformed;
	}
	return point;
}

enum sippymodem_v32_constellation_result
sippymodem_v32_trellis_constellation_point(
	uint8_t data_bits_per_symbol, uint8_t signal_symbol,
	struct sippymodem_qam_point *point)
{
	struct doubled_grid_point grid_point;
	struct sippymodem_qam_point next;
	uint8_t low_bit_count;
	uint8_t low_mask;
	uint8_t subset;
	double normalization;

	if (!trellis_data_bits_are_valid(data_bits_per_symbol) ||
	    signal_symbol >= (UINT8_C(1) << (data_bits_per_symbol + 1u)) ||
	    point == NULL)
		return SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT;
	low_bit_count = (uint8_t)(data_bits_per_symbol - 2u);
	low_mask = (uint8_t)((UINT8_C(1) << low_bit_count) - 1u);
	subset = (uint8_t)(signal_symbol >> low_bit_count);
	grid_point = trellis_seed_point(
		data_bits_per_symbol,
		subset == 2 || subset == 3 || subset == 4 || subset == 5,
		(uint8_t)(signal_symbol & low_mask));
	grid_point = trellis_subset_transform(subset, grid_point);
	/* Preserve one common line-amplitude scale at every rate. */
	normalization = 2.0 * sqrt(18.0);
	next = (struct sippymodem_qam_point){
		.in_phase = grid_point.in_phase / normalization,
		.quadrature = grid_point.quadrature / normalization,
	};
	*point = next;
	return SIPPYMODEM_V32_CONSTELLATION_OK;
}

enum sippymodem_v32_constellation_result
sippymodem_v32_7200_constellation_point(
	uint8_t signal_symbol, struct sippymodem_qam_point *point)
{
	return sippymodem_v32_trellis_constellation_point(
		3, signal_symbol, point);
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
