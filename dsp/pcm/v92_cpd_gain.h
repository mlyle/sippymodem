// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_GAIN_H
#define V92_CPD_GAIN_H

#include <stddef.h>
#include <stdint.h>

/* CPd carries F=4G in Q0.16, so G=F/2^18. Choose the representable
 * F whose scaled mean squared prefilter output is nearest unity. */
static inline uint16_t v92_cpd_gain_for_energy(uint64_t energy, size_t symbols)
{
	uint32_t low = 0u, high = UINT16_MAX;
	__uint128_t target = (__uint128_t)symbols << 36;

	if (energy == 0u || symbols == 0u)
		return 0u;
	while (low < high) {
		uint32_t middle = low + (high - low + 1u) / 2u;

		if ((__uint128_t)middle * middle * energy <= target)
			low = middle;
		else
			high = middle - 1u;
	}
	if (low == 0u)
		return 1u;
	if (low < UINT16_MAX) {
		uint32_t upper = low + 1u;
		__uint128_t below = target - (__uint128_t)low * low * energy;
		__uint128_t above = (__uint128_t)upper * upper * energy - target;

		if (above < below)
			low = upper;
	}
	return (uint16_t)low;
}

#endif /* V92_CPD_GAIN_H */
