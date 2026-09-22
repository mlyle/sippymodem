// SPDX-License-Identifier: BSD-3-Clause

#ifndef FIR_H
#define FIR_H

#include <stddef.h>
#include <stdint.h>

/* Shift a newest-first history and return the low 32 bits of its signed
 * convolution. The two count-element arrays must be separate; count zero
 * leaves history untouched and returns zero. Scaling/rounding belong to the
 * caller. No extra sample delay or history is introduced. */
static inline uint32_t fir_shift_s16(
	int16_t *restrict history, const int16_t *restrict coefficients,
	size_t count, int16_t sample)
{
	uint32_t sum = 0;

	for (size_t tap = 0; tap < count; ++tap) {
		int16_t previous = history[tap];

		history[tap] = sample;
		sum += (uint32_t)((int32_t)sample * coefficients[tap]);
		sample = previous;
	}
	return sum;
}

#endif
