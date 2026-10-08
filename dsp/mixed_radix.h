// SPDX-License-Identifier: BSD-3-Clause

#ifndef MIXED_RADIX_H
#define MIXED_RADIX_H

#include <stddef.h>
#include <stdint.h>

enum {
	MIXED_RADIX_MAX_BITS = 128,
	MIXED_RADIX_MAX_DIGITS = 128,
};

enum mixed_radix_result {
	MIXED_RADIX_OK,
	MIXED_RADIX_INVALID_ARGUMENT,
	MIXED_RADIX_INVALID_CONFIGURATION,
	MIXED_RADIX_OUT_OF_RANGE,
};

/* Bits are least-significant first. Rejected calls leave digits unchanged. */
enum mixed_radix_result mixed_radix_encode(
	const uint32_t *radices, size_t digit_count,
	const uint8_t *bits, size_t bit_count,
	uint32_t *digits);

/* Recover least-significant-first bits. Tuples outside the configured binary
 * input range are rejected without changing bits. */
enum mixed_radix_result mixed_radix_decode(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *bits, size_t bit_count);

/* Demodulated digits may form an unused tuple after a channel error. Recover
 * its low bits so the receiver can keep its frame and descrambler clocks.
 * Invalid digits/configurations are still rejected without changing bits. */
enum mixed_radix_result mixed_radix_decode_low_bits(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *bits, size_t bit_count);

/* Differential framing reflects every digit when the preceding frame sign is
 * one. State and outputs change together only after a valid mapping. */
enum mixed_radix_result mixed_radix_encode_differential(
	const uint32_t *radices, size_t digit_count,
	const uint8_t *bits, size_t bit_count,
	uint8_t *differential_state, uint32_t *digits,
	uint8_t *frame_sign);
enum mixed_radix_result mixed_radix_decode_differential(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *differential_state,
	uint8_t *bits, size_t bit_count);


#endif /* MIXED_RADIX_H */
