// SPDX-License-Identifier: BSD-3-Clause

#ifndef WIDE_UINT_H
#define WIDE_UINT_H

#include <stdbool.h>
#include <stdint.h>

/* Unsigned 128-bit arithmetic, modulo 2^128. Checked operations return false
 * on overflow and still store the low word. No compiler extension is required;
 * compilers with native support keep their ordinary arithmetic fast path. */
struct wide_uint {
	uint64_t high, low;
};

static inline struct wide_uint wide_from_u64(uint64_t value)
{
	return (struct wide_uint){ .low = value };
}

static inline int wide_compare(struct wide_uint left, struct wide_uint right)
{
	if (left.high != right.high)
		return left.high > right.high ? 1 : -1;
	return (left.low > right.low) - (left.low < right.low);
}

static inline bool wide_add(struct wide_uint left, struct wide_uint right,
	struct wide_uint *result)
{
	uint64_t low = left.low + right.low;
	uint64_t high = left.high + right.high;
	bool overflow = high < left.high;
	uint64_t carry = low < left.low;

	*result = (struct wide_uint){ high + carry, low };
	return !overflow && result->high >= high;
}

static inline struct wide_uint wide_subtract(
	struct wide_uint left, struct wide_uint right)
{
	return (struct wide_uint){
		left.high - right.high - (left.low < right.low),
		left.low - right.low,
	};
}

/* Shifts of 128 or more yield zero; zero and 64 are defined explicitly. */
static inline struct wide_uint wide_shift_left(struct wide_uint value, unsigned bits)
{
	if (bits >= 128)
		return wide_from_u64(0);
	if (bits >= 64)
		return (struct wide_uint){ value.low << (bits - 64), 0 };
	if (bits == 0)
		return value;
	return (struct wide_uint){
		(value.high << bits) | (value.low >> (64 - bits)), value.low << bits,
	};
}

static inline struct wide_uint wide_shift_right(struct wide_uint value, unsigned bits)
{
	if (bits >= 128)
		return wide_from_u64(0);
	if (bits >= 64)
		return wide_from_u64(value.high >> (bits - 64));
	if (bits == 0)
		return value;
	return (struct wide_uint){
		value.high >> bits, (value.low >> bits) | (value.high << (64 - bits)),
	};
}

#if defined(__SIZEOF_INT128__) && !defined(WIDE_UINT_PORTABLE)
static inline __uint128_t wide_native(struct wide_uint value)
{
	return ((__uint128_t)value.high << 64) | value.low;
}

static inline struct wide_uint wide_from_native(__uint128_t value)
{
	return (struct wide_uint){ (uint64_t)(value >> 64), (uint64_t)value };
}
#endif

/* A 64-by-64 product is exact. Full-width products retain the low 128 bits. */
static inline struct wide_uint wide_product_u64(uint64_t left, uint64_t right)
{
#if defined(__SIZEOF_INT128__) && !defined(WIDE_UINT_PORTABLE)
	return wide_from_native((__uint128_t)left * right);
#else
	uint64_t a0 = (uint32_t)left, a1 = left >> 32;
	uint64_t b0 = (uint32_t)right, b1 = right >> 32;
	uint64_t product = a0 * b0;
	uint32_t low = (uint32_t)product;
	uint64_t middle = a1 * b0 + (product >> 32);
	uint64_t high = middle >> 32;
	middle = a0 * b1 + (uint32_t)middle;
	return (struct wide_uint){
		a1 * b1 + high + (middle >> 32), (middle << 32) | low,
	};
#endif
}

static inline struct wide_uint wide_multiply(struct wide_uint left, struct wide_uint right)
{
#if defined(__SIZEOF_INT128__) && !defined(WIDE_UINT_PORTABLE)
	return wide_from_native(wide_native(left) * wide_native(right));
#else
	struct wide_uint result = wide_product_u64(left.low, right.low);
	result.high += left.high * right.low + left.low * right.high;
	return result;
#endif
}

static inline bool wide_multiply_u32(struct wide_uint value, uint32_t factor,
	struct wide_uint *result)
{
#if defined(__SIZEOF_INT128__) && !defined(WIDE_UINT_PORTABLE)
	__uint128_t product;
	bool overflow = __builtin_mul_overflow(wide_native(value), factor, &product);
	*result = wide_from_native(product);
	return !overflow;
#else
	uint32_t words[4] = {
		(uint32_t)value.low, (uint32_t)(value.low >> 32),
		(uint32_t)value.high, (uint32_t)(value.high >> 32),
	};
	uint64_t carry = 0;
	for (unsigned index = 0; index < 4; ++index) {
		uint64_t product = (uint64_t)words[index] * factor + carry;
		words[index] = (uint32_t)product;
		carry = product >> 32;
	}
	*result = (struct wide_uint){
		((uint64_t)words[3] << 32) | words[2],
		((uint64_t)words[1] << 32) | words[0],
	};
	return carry == 0;
#endif
}

/* divisor must be nonzero. Store the quotient and return the remainder. */
static inline uint32_t wide_divide_u32(struct wide_uint value, uint32_t divisor,
	struct wide_uint *quotient)
{
#if defined(__SIZEOF_INT128__) && !defined(WIDE_UINT_PORTABLE)
	__uint128_t native = wide_native(value);
	*quotient = wide_from_native(native / divisor);
	return (uint32_t)(native % divisor);
#else
	uint32_t words[4] = {
		(uint32_t)value.low, (uint32_t)(value.low >> 32),
		(uint32_t)value.high, (uint32_t)(value.high >> 32),
	};
	uint64_t remainder = 0;
	for (unsigned index = 4; index != 0; --index) {
		uint64_t dividend = (remainder << 32) | words[index - 1];
		words[index - 1] = (uint32_t)(dividend / divisor);
		remainder = dividend % divisor;
	}
	*quotient = (struct wide_uint){
		((uint64_t)words[3] << 32) | words[2],
		((uint64_t)words[1] << 32) | words[0],
	};
	return (uint32_t)remainder;
#endif
}

#endif
