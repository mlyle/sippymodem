// SPDX-License-Identifier: BSD-3-Clause

#ifndef FIR_H
#define FIR_H

#include <stddef.h>
#include <stdint.h>

/* Read a signed 32-bit history and 16-bit coefficient convolution without
 * scaling, rounding or changing either array. Each product is exact; the
 * caller must bound count and values so every partial sum fits int64_t. Count zero
 * accesses neither array and returns zero. */
static inline int64_t fir_read_s32_s16(
	const int32_t *history, const int16_t *coefficients, size_t count)
{
	int64_t sum = 0;

	for (size_t tap = 0; tap < count; ++tap)
		sum += (int64_t)history[tap] * coefficients[tap];
	return sum;
}

/* Add one signed complex sample/coefficient product to separate 32-bit
 * accumulator words. Each multiplication is exact; sums wrap modulo 2^32,
 * including the full-scale imaginary product 2^31. The caller owns scaling,
 * rounding, and tap/history order. Both accumulator pointers must be valid
 * and distinct. */
static inline void fir_accumulate_complex_s16(
	int16_t input_real, int16_t input_imag,
	int16_t coefficient_real, int16_t coefficient_imag,
	uint32_t *real, uint32_t *imag)
{
	*real += (uint32_t)((int32_t)input_real * coefficient_real) -
		 (uint32_t)((int32_t)input_imag * coefficient_imag);
	*imag += (uint32_t)((int32_t)input_real * coefficient_imag) +
		 (uint32_t)((int32_t)input_imag * coefficient_real);
}

/* Subtract conjugate(reference) * error from two coefficient accumulator
 * words. Products are exact; sums wrap modulo 2^32 even for full-scale
 * negative inputs. The caller owns scaling, rounding and retained residue.
 * Both accumulator pointers must be valid and distinct. */
static inline void fir_subtract_correlation_s16(
	int16_t reference_real, int16_t reference_imag,
	int16_t error_real, int16_t error_imag,
	uint32_t *real, uint32_t *imag)
{
	*real -= (uint32_t)((int32_t)reference_real * error_real);
	*real -= (uint32_t)((int32_t)reference_imag * error_imag);
	*imag -= (uint32_t)((int32_t)reference_real * error_imag);
	*imag += (uint32_t)((int32_t)reference_imag * error_real);
}

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

/* Read count taps from a circular history, starting with its newest sample
 * at index and walking backwards. Coefficients advance by stride (at least
 * one), allowing a column of a polyphase bank without copying it. Return the
 * low 32 bits of the signed convolution; scaling belongs to the caller.
 * For nonzero count, index must be less than count and both arrays must cover
 * their respective spans. Count zero accesses neither array. No state changes. */
static inline uint32_t fir_read_ring_s16(
	const int16_t *history, const int16_t *coefficients,
	size_t count, size_t index, size_t stride)
{
	uint32_t sum = 0;

	for (size_t tap = 0; tap < count; ++tap) {
		sum += (uint32_t)((int32_t)history[index] * coefficients[tap * stride]);
		index = index == 0 ? count - 1 : index - 1;
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
