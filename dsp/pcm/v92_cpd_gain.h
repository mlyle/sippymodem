// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_GAIN_H
#define V92_CPD_GAIN_H

#include "wide_uint.h"

#include <stddef.h>
#include <stdint.h>

/* CPd carries F=4G in Q0.16, so G=F/2^18. Choose the representable
 * F whose scaled mean squared prefilter output is nearest unity. */
static inline uint16_t v92_cpd_gain_for_energy(uint64_t energy, size_t symbols)
{
	uint32_t low = 0u, high = UINT16_MAX;
	struct wide_uint target = wide_shift_left(wide_from_u64(symbols), 36);

	if (energy == 0u || symbols == 0u)
		return 0u;
	while (low < high) {
		uint32_t middle = low + (high - low + 1u) / 2u;

		if (wide_compare(wide_product_u64((uint64_t)middle * middle, energy), target) <= 0)
			low = middle;
		else
			high = middle - 1u;
	}
	if (low == 0u)
		return 1u;
	if (low < UINT16_MAX) {
		uint32_t upper = low + 1u;
		struct wide_uint below = wide_subtract(target,
			wide_product_u64((uint64_t)low * low, energy));
		struct wide_uint above = wide_subtract(
			wide_product_u64((uint64_t)upper * upper, energy), target);

		if (wide_compare(above, below) < 0)
			low = upper;
	}
	return (uint16_t)low;
}

#endif /* V92_CPD_GAIN_H */
