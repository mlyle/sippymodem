// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CONTROL_FRAME_H
#define V92_CONTROL_FRAME_H

#include <stdbool.h>

enum {
	V92_CONTROL_UPSTREAM_4_POINT_BITS = 24,
	V92_CONTROL_UPSTREAM_8_POINT_BITS = 36,
	V92_CONTROL_UPSTREAM_DATA_MIN_BITS = 36,
	V92_CONTROL_UPSTREAM_DATA_MAX_BITS = 72,
};

/* Twelve upstream symbols carry 24/36 bits during TRN2u, or an even
 * 36..72 bits in data mode. A data frame need not have an integer number
 * of input bits per symbol. */
static inline bool v92_control_upstream_alignment_is_valid(unsigned int bits)
{
	return bits == V92_CONTROL_UPSTREAM_4_POINT_BITS ||
	       (bits >= V92_CONTROL_UPSTREAM_DATA_MIN_BITS &&
		bits <= V92_CONTROL_UPSTREAM_DATA_MAX_BITS && bits % 2u == 0u);
}

#endif /* V92_CONTROL_FRAME_H */
