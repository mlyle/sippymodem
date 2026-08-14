// SPDX-License-Identifier: BSD-3-Clause

#ifndef INTEGER_MATH_H
#define INTEGER_MATH_H

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

#endif
