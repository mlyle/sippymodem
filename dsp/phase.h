// SPDX-License-Identifier: BSD-3-Clause

#ifndef DSP_PHASE_H
#define DSP_PHASE_H

#include <stdint.h>

/* A full turn spans every value of an unsigned 32-bit accumulator. */
static inline uint32_t phase_step_u32(
	uint32_t sample_rate, uint32_t frequency_numerator,
	uint32_t frequency_denominator)
{
	uint64_t numerator;
	uint64_t denominator;
	uint64_t quotient;
	uint64_t remainder;

	if (sample_rate == 0 || frequency_numerator == 0 ||
	    frequency_denominator == 0)
		return 0;
	numerator = (uint64_t)frequency_numerator << 32;
	denominator = (uint64_t)sample_rate * frequency_denominator;
	quotient = numerator / denominator;
	remainder = numerator % denominator;
	if (remainder >= (denominator + 1u) / 2u)
		quotient++;
	return quotient <= UINT32_MAX ? (uint32_t)quotient : 0;
}

static inline double phase_radians_u32(uint32_t phase)
{
	return (double)phase *
	       (6.283185307179586476925286766559 / 4294967296.0);
}

#endif
