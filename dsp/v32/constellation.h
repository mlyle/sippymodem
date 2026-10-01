// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONSTELLATION_H
#define V32_CONSTELLATION_H

#include "qam.h"

#include <stdint.h>

enum {
	V32_CONSTELLATION_GRID_UNIT = 4096,
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

/* Integer signal-point domain used by the V.32 receiver and encoder. */
struct v32_fixed_point {
	int16_t i;
	int16_t q;
};

/* Generate the normalized 4-by-4 V.32 signal-point geometry. */
enum v32_constellation_result
v32_constellation_point(
	uint8_t symbol, struct qam_point *point);

enum v32_constellation_result
v32_constellation_fixed_point(
	uint8_t symbol, struct v32_fixed_point *point);

/* Return the four constant-energy points used at 4800 bit/s. */
enum v32_constellation_result
v32_4800_constellation_point(
	uint8_t quadrant, struct qam_point *point);

enum v32_constellation_result
v32_4800_fixed_point(
	uint8_t quadrant, struct v32_fixed_point *point);

/* Generate the set-partitioned 16-point labeling used at 7200 bit/s. */
enum v32_constellation_result
v32_7200_constellation_point(
	uint8_t signal_symbol, struct qam_point *point);

/*
 * Generate a set-partitioned trellis signal point.  The data group contains
 * three through six bits for 7200 through 14400 bit/s respectively, including
 * four bits at 9600 bit/s; the trellis encoder adds one signal-label bit.
 */
enum v32_constellation_result
v32_trellis_constellation_point(
	uint8_t data_bits_per_symbol, uint8_t signal_symbol,
	struct qam_point *point);

/* Generate the fixed TX/VTB signal-label point.  A receiver feedback-point
 * index is a separate domain and must not be passed as a signal symbol. */
enum v32_constellation_result
v32_trellis_fixed_signal_point(
	uint8_t data_bits_per_symbol, uint8_t signal_symbol,
	struct v32_fixed_point *point);

#endif
