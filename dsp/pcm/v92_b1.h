// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_B1_H
#define V92_B1_H

#include "v92_pcm_mapping.h"
#include "v34_framing.h"

#include <math.h>
#include <string.h>

/* Advance known source ones through an owned scrambler and mixed-radix
 * mapper. The mapping is already validated, state and outputs are disjoint,
 * and the caller owns the coding epoch. Trellis and precoder choices stay
 * with the frame owner; these digits describe their permitted classes. */
static inline bool v92_b1_digits(
	const struct v92_pcm_mapping *mapping, struct v34_scrambler *scrambler,
	uint8_t *differential, uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS], sign;
	memset(bits, 1, mapping->data_bits_per_frame);
	return v34_scramble_unpacked_bits(scrambler, bits,
		mapping->data_bits_per_frame, bits) == V34_FRAMING_OK &&
	       v92_pcm_mapping_encode_owned(mapping, bits,
		mapping->data_bits_per_frame, differential, digits, &sign) == V92_PCM_MAPPING_OK;
}

/* Squared distance to the nearest representative of one ordinary-symbol
 * residue class. A validated constellation and modulus contain at least one
 * representative for digit < modulus; the finite observation has a finite
 * squared residual. Fourth-symbol trellis cosets are selected by the owner. */
static inline double v92_b1_class_error(
	const struct v92_cpd_constellation *points, unsigned modulus,
	unsigned digit, double observed)
{
	int first = -(int)points->count;
	first += (int)((digit + points->count) % modulus);
	double best = INFINITY;
	for (int eta = first; eta < points->count; eta += (int)modulus) {
		double error = observed - v92_cpd_signed_point(points, eta);
		best = fmin(best, error * error);
	}
	return best;
}

#endif
