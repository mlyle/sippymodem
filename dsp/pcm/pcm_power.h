// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_POWER_H
#define PCM_POWER_H
#include <stdint.h>

/* Exact numerator / 2^bits, split before multiplication. Callers own the
 * accumulator and supply bits <= 42, weight <= 2^bits and squares < 2^37
 * (at most 128 squared 16-bit codec levels). Upper < 2^63 and lower < 2^59;
 * Six phases of either individual levels or three prefix terms fit exactly.
 * No floating rounding or compiler-specific wide integer is required. */
struct pcm_power_sum {
	uint64_t whole, fraction;
};

static inline void pcm_power_accumulate(struct pcm_power_sum *sum,
	unsigned bits, uint64_t weight, uint64_t squares)
{
	uint64_t mask = (UINT64_C(1) << bits) - 1u;
	uint64_t upper = weight * (squares >> 16);
	uint64_t lower = weight * (squares & UINT16_MAX) + ((upper & mask) << 16);
	sum->whole += ((upper >> bits) << 16) + (lower >> bits);
	sum->fraction += lower & mask;
}

/* Normalize six phases whose mean square fits uint32_t. */
static inline void pcm_power_bounds(const struct pcm_power_sum *sum,
	unsigned bits, uint32_t *floor, uint32_t *ceiling)
{
	uint64_t whole = sum->whole + (sum->fraction >> bits);
	*floor = (uint32_t)(whole / 6u);
	*ceiling = *floor + (whole % 6u != 0u ||
		(sum->fraction & ((UINT64_C(1) << bits) - 1u)) != 0u);
}

/* V.90 Table 15 mean-square limits in 16-bit linear PCM units. V.91 uses
 * the same entries for codes 1..31; its code zero means unspecified. */
static inline uint32_t pcm_power_limit(uint8_t code)
{
	static const uint16_t rms[] = {
		15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108,
		9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380,
		6020, 5684, 5368, 5068, 4784, 4516, 4264, 4024,
		3800, 3588, 3388, 3196, 3020, 2852, 2692, 2540,
	};
	return code < 32 ? (uint32_t)rms[code] * rms[code] : 0;
}
#endif
