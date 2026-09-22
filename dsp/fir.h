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

/* Insert one sample into an even-length symmetric FIR ring and return its
 * unscaled signed convolution. count must be positive and even, *index must
 * name the next slot to overwrite, and the sum must fit int64_t. coefficients
 * contains count/2 taps, ordered from the outer pair toward the centre.
 * History, coefficients, and index must be separate. Rounding belongs to the
 * caller; the new sample contributes immediately, without extra delay. */
static inline int64_t fir_ring_symmetric_s16(
	int16_t *restrict history, const int16_t *restrict coefficients,
	size_t count, size_t *index, int16_t sample)
{
	size_t recent = *index;
	size_t oldest = recent + 1 == count ? 0 : recent + 1;
	int64_t sum = 0;

	history[recent] = sample;
	*index = oldest;
	for (size_t tap = 0; tap < count / 2; ++tap) {
		sum += (int64_t)coefficients[tap] *
		       ((int32_t)history[recent] + history[oldest]);
		recent = recent == 0 ? count - 1 : recent - 1;
		if (++oldest == count)
			oldest = 0;
	}
	return sum;
}

#endif
