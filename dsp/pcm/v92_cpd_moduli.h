// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_MODULI_H
#define V92_CPD_MODULI_H

#include "v92_cpd.h"
#include "wide_uint.h"

_Static_assert(V92_CPD_MODULUS_COUNT == 12, "balanced capacity assumes twelve symbols");

/* Minimum positive points for one constellation shared by all twelve
 * intervals. Every fourth symbol also carries the trellis coset bit.
 * With no precoder memory, outer representatives cannot reduce |x|. */
static inline uint8_t v92_cpd_minimum_points(
	const uint8_t moduli[V92_CPD_MODULUS_COUNT])
{
	uint8_t count = 0;

	for (unsigned symbol = 0; symbol < V92_CPD_MODULUS_COUNT; ++symbol) {
		unsigned required = symbol % 4u == 3u ? moduli[symbol] :
			(moduli[symbol] + 1u) / 2u;

		if (required > count)
			count = (uint8_t)required;
	}
	return count;
}

/* Nine ordinary moduli are 2N and three trellis moduli are N. Their
 * product gives capacity = (2N)^9 N^3 = 512 (N^6)^2. For 1 <= N <= 64,
 * N^3 fits uint32_t, N^6 fits uint64_t and capacity fits 82 bits. */
static inline struct wide_uint v92_cpd_balanced_capacity(uint32_t points)
{
	uint32_t cube = points * points * points;
	uint64_t sixth = (uint64_t)cube * cube;

	return wide_shift_left(wide_product_u64(sixth, sixth), 9);
}

/* Choose the smallest balanced alphabet carrying the selected rate. Some
 * peers infer K from floor(log2(product(M))); reduce only the first ordinary
 * modulus until that width equals K. Selection of this family, constellation
 * levels and trellis remain with the designer. Rejection changes nothing. */
static inline bool v92_cpd_balanced_moduli(
	uint8_t moduli[V92_CPD_MODULUS_COUNT], uint8_t rate)
{
	unsigned bits = pcm_upstream_frame_bits(rate);
	unsigned low = 1, high = 64;

	if (moduli == NULL || !bits) return false;
	struct wide_uint required = wide_shift_left(wide_from_u64(1), bits);
	struct wide_uint upper = wide_shift_left(required, 1);
	while (low < high) {
		unsigned middle = low + (high - low) / 2;
		if (wide_compare(v92_cpd_balanced_capacity(middle), required) < 0)
			low = middle + 1;
		else
			high = middle;
	}
	unsigned first = 2 * low;
	struct wide_uint capacity = v92_cpd_balanced_capacity(low);
	while (wide_compare(capacity, upper) >= 0 && first > 1) {
		wide_divide_u32(capacity, first, &capacity);
		wide_multiply_u32(capacity, --first, &capacity);
	}
	if (wide_compare(capacity, required) < 0 || wide_compare(capacity, upper) >= 0)
		return false;
	for (unsigned symbol = 0; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
		moduli[symbol] = symbol % 4 == 3 ? low : 2 * low;
	moduli[0] = first;
	return true;
}

#endif
