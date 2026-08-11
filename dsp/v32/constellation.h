// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONSTELLATION_H
#define V32_CONSTELLATION_H

#include "qam.h"

#include <stdint.h>

enum {
	V32_CONSTELLATION_SYMBOLS = 16,
	V32_4800_SIGNAL_POINTS = 4,
	V32_7200_SIGNAL_POINTS = 16,
	V32_9600_TRELLIS_SIGNAL_POINTS = 32,
	V32_12000_SIGNAL_POINTS = 64,
	V32_14400_SIGNAL_POINTS = 128,
};

enum v32_constellation_result {
	V32_CONSTELLATION_OK,
	V32_CONSTELLATION_INVALID_ARGUMENT,
};

/* Generate the normalized 4-by-4 V.32 signal-point geometry. */
enum v32_constellation_result
v32_constellation_point(
	uint8_t symbol, struct qam_point *point);

/* Return the four constant-energy points used at 4800 bit/s. */
enum v32_constellation_result
v32_4800_constellation_point(
	uint8_t quadrant, struct qam_point *point);

/* Generate the set-partitioned 16-point labeling used at 7200 bit/s. */
enum v32_constellation_result
v32_7200_constellation_point(
	uint8_t signal_symbol, struct qam_point *point);

/*
 * Generate a set-partitioned trellis signal point.  The data group contains
 * three through six bits for 7200 through 14400 bit/s respectively; the
 * trellis encoder adds one signal-label bit.
 */
enum v32_constellation_result
v32_trellis_constellation_point(
	uint8_t data_bits_per_symbol, uint8_t signal_symbol,
	struct qam_point *point);

const char *v32_constellation_result_name(
	enum v32_constellation_result result);

#endif
