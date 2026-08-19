// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_DIRECT_H
#define V92_CPD_DIRECT_H

#include "g711.h"
#include "v92_cpd.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* The direct profile owns one four-tap filter and one constellation
	 * containing at most 64 positive levels. Keep its wire owner bounded by
	 * that generated profile rather than the protocol-wide CPd maximum. */
	V92_CPD_DIRECT_FILTER_TAPS = 4,
	V92_CPD_DIRECT_LINEAR_POINT_LIMIT = 16,
	V92_CPD_DIRECT_MAX_POINTS = 64,
	V92_CPD_DIRECT_MAX_FRAME_BITS =
		V92_CPD_FIXED_BITS + V92_CPD_MODULUS_BITS +
		V92_CPD_FILTER_HEADER_BITS +
		V92_CPD_DIRECT_FILTER_TAPS * V92_CPD_WORD_BITS +
		V92_CPD_CONSTELLATION_HEADER_BITS +
		V92_CPD_DIRECT_MAX_POINTS * V92_CPD_WORD_BITS +
		V92_CPD_TRAILER_BITS +
		V92_CPD_DOWNSTREAM_FRAME_MAX_BITS - 1,
	V92_CPD_DIRECT_MAX_FRAME_BYTES =
		(V92_CPD_DIRECT_MAX_FRAME_BITS + 7) / 8,
};

/* Build the complete initial CPd for a direct PCM-upstream mapping. The
 * generated modulus, filter, and constellation sections are owned by the
 * result; later parameter-mode updates may use the compact CPd form.
 * Rejected calls leave parameters unchanged. */
enum v92_cpd_result v92_cpd_direct_init(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, bool extend_e2u);

#endif /* V92_CPD_DIRECT_H */
