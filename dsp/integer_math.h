// SPDX-License-Identifier: BSD-3-Clause

#ifndef INTEGER_MATH_H
#define INTEGER_MATH_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

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
