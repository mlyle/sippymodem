// SPDX-License-Identifier: BSD-3-Clause

#ifndef INTEGER_MATH_H
#define INTEGER_MATH_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* Count set bits by summing adjacent pairs, then nibbles. */
static inline uint8_t bit_count_u8(uint8_t bits)
{
	bits = (uint8_t)((bits & 0x55u) + ((bits >> 1) & 0x55u));
	bits = (uint8_t)((bits & 0x33u) + ((bits >> 2) & 0x33u));
	return (uint8_t)((bits + (bits >> 4)) & 0x0fu);
}

/* Number of significant bits, including a defined zero result for zero. */
static inline unsigned int bit_width_unsigned(unsigned int value)
{
	return value ? (unsigned int)(sizeof(value) * CHAR_BIT) -
		       (unsigned int)__builtin_clz(value) : 0u;
}

/* Interpret the low fixed-width word without relying on implementation-defined
 * unsigned-to-signed conversions. */
static inline int32_t wrap_s32_from_u32(uint32_t value)
{
	return value <= INT32_MAX ?
		       (int32_t)value :
		       (int32_t)((int64_t)value - (INT64_C(1) << 32));
}

static inline int64_t wrap_s64_from_u64(uint64_t value)
{
	return value <= INT64_MAX ?
		       (int64_t)value :
		       -(int64_t)(UINT64_MAX - value) - 1;
}

static inline int16_t wrap_s16_from_s32(int32_t value)
{
	uint16_t word = (uint16_t)(uint32_t)value;

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

/* Assemble and read a signed high word with an unsigned low residue. */
static inline uint32_t pack_s16_u16_words(int16_t high, uint16_t low)
{
	return ((uint32_t)(uint16_t)high << 16) | low;
}

static inline int16_t high_s16_word(uint32_t value)
{
	return wrap_s16_from_s32((int32_t)(value >> 16));
}

static inline int32_t magnitude_s16(int16_t value)
{
	return value < 0 ? -(int32_t)value : value;
}

static inline uint64_t magnitude_s64(int64_t value)
{
	return value < 0 ? UINT64_C(0) - (uint64_t)value : (uint64_t)value;
}

static inline uint64_t square_s16(int16_t value)
{
	int64_t widened = value;

	return (uint64_t)(widened * widened);
}

static inline uint64_t add_u64_saturating(uint64_t left, uint64_t right)
{
	return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static inline int64_t add_s64_saturating(int64_t left, int64_t right)
{
	if (right > 0 && left > INT64_MAX - right)
		return INT64_MAX;
	if (right < 0 && left < INT64_MIN - right)
		return INT64_MIN;
	return left + right;
}

/* Return the low two's-complement word of a signed product. */
static inline uint32_t multiply_low_s32(int32_t left, int32_t right)
{
	return (uint32_t)((uint64_t)(uint32_t)left * (uint32_t)right);
}

/* Preserve the low signed word, including INT16_MIN for abs(INT16_MIN). */
static inline int16_t absolute_wrap_s16(int16_t value)
{
	return wrap_s16_from_s32(magnitude_s16(value));
}

static inline int16_t clamp_s16(int64_t value)
{
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

/* Arithmetic right shift of a two's-complement word, defined for every shift. */
static inline int32_t arithmetic_shift_right_u32(
	uint32_t value, unsigned int shift)
{
	uint32_t shifted;

	if (shift == 0u)
		shifted = value;
	else if (shift >= 32u)
		shifted = (value & UINT32_C(0x80000000)) != 0 ?
			UINT32_MAX : 0u;
	else if ((value & UINT32_C(0x80000000)) == 0)
		shifted = value >> shift;
	else
		shifted = (value >> shift) | ~(UINT32_MAX >> shift);
	return wrap_s32_from_u32(shifted);
}

static inline int64_t arithmetic_shift_right_u64(
	uint64_t value, unsigned int shift)
{
	uint64_t shifted;

	if (shift == 0u)
		shifted = value;
	else if (shift >= 64u)
		shifted = (value & (UINT64_C(1) << 63)) != 0 ?
			UINT64_MAX : 0u;
	else if ((value & (UINT64_C(1) << 63)) == 0)
		shifted = value >> shift;
	else
		shifted = (value >> shift) | ~(UINT64_MAX >> shift);
	return wrap_s64_from_u64(shifted);
}

/* Round a signed low word upward at ties and keep the low 16 result bits. */
static inline int16_t round_shift_s32_word_to_s16(
	uint32_t value, unsigned int shift)
{
	uint32_t rounding = shift > 0u && shift < 32u ?
		UINT32_C(1) << (shift - 1u) : 0u;
	return wrap_s16_from_s32(
		arithmetic_shift_right_u32(value + rounding, shift));
}

static inline int16_t multiply_shift_s16(
	int16_t left, int16_t right, unsigned int shift)
{
	return wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)left * right), shift));
}

/* Quantized reciprocal profile, not a mathematically exact reciprocal.
 * value must be nonzero and exponent writable. Return a Q15 mantissa and
 * store the 0..15 left shifts that normalize value's leading one to bit 15.
 * Round the normalized high byte upward at ties (128..256), then truncate
 * 2^22 divided by that value; the returned mantissa is 16384..32768. */
static inline uint16_t normalized_reciprocal_q15(
	uint16_t value, uint16_t *exponent)
{
	uint16_t normalized = value;
	uint16_t shift = 0;
	uint16_t high;

	while ((normalized & UINT16_C(0x8000)) == 0) {
		normalized = (uint16_t)(normalized << 1);
		shift++;
	}
	high = (uint16_t)(((uint32_t)normalized + 0x80u) >> 8);
	*exponent = shift;
	return (uint16_t)((UINT32_C(1) << 22) / high);
}

/* Compare value * scale >= bound without overflowing the product.
 * scale must be positive; even the rounded-up quotient fits uint64_t. */
static inline bool scaled_u64_at_least(
	uint64_t value, unsigned scale, uint64_t bound)
{
	return value >= bound / scale + (bound % scale != 0);
}

/* Euclid's algorithm, with gcd(0, 0) defined as zero. */
static inline uint64_t gcd_u64(uint64_t left, uint64_t right)
{
	while (right != 0) {
		uint64_t remainder = left % right;

		left = right;
		right = remainder;
	}
	return left;
}

/* Round half away from zero without negating INT64_MIN. */
static inline int64_t round_shift_s64(int64_t value, unsigned int shift)
{
	uint64_t magnitude;
	bool negative;

	if (shift == 0u)
		return value;
	if (shift > 64u)
		return 0;
	negative = value < 0;
	magnitude = negative ? UINT64_C(0) - (uint64_t)value :
			       (uint64_t)value;
	if (shift == 64u)
		magnitude = magnitude >= (UINT64_C(1) << 63) ? 1u : 0u;
	else
		magnitude = (magnitude +
			     (UINT64_C(1) << (shift - 1u))) >> shift;
	return negative ? -(int64_t)magnitude : (int64_t)magnitude;
}

static inline int32_t round_shift_s64_to_s32_saturating(
	int64_t value, unsigned int shift)
{
	int64_t rounded = round_shift_s64(value, shift);

	if (rounded > INT32_MAX)
		return INT32_MAX;
	if (rounded < INT32_MIN)
		return INT32_MIN;
	return (int32_t)rounded;
}

static inline int16_t round_shift_s64_to_s16_saturating(
	int64_t value, unsigned int shift)
{
	return clamp_s16(round_shift_s64(value, shift));
}

static inline uint32_t integer_square_root_u32(uint32_t value)
{
	uint32_t root = 0;
	uint32_t bit = UINT32_C(1) << 30;

	while (bit > value)
		bit >>= 2;
	while (bit != 0) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

/* Exact formula for the modem's normalized fixed-point square-root profile. */
static inline uint16_t fixed_square_root_u32(uint32_t value)
{
	uint16_t shift = 0;
	uint32_t normalized = value;
	uint16_t mantissa;
	uint16_t index;
	uint32_t profile_value;

	if (value == 0)
		return 0;
	while (normalized <= UINT32_C(0x1fffffff)) {
		normalized <<= 1;
		shift++;
	}
	mantissa = (uint16_t)(normalized >> 15);
	if ((shift & 1u) != 0)
		mantissa >>= 1;
	index = (uint16_t)((((uint32_t)mantissa + 0x40u) >> 7) -
			   0x40u);
	if (index > 0xbfu)
		index = 0xbfu;
	profile_value = ((uint32_t)index + 64u) << 22;
	return (uint16_t)(integer_square_root_u32(profile_value) >>
			  (shift >> 1));
}

#endif
