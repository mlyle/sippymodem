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

#endif
