// SPDX-License-Identifier: BSD-3-Clause

#include "mixed_radix.h"
#include "wide_uint.h"

#include <stdbool.h>
#include <string.h>

static bool configuration_is_valid(
	const uint32_t *radices, size_t digit_count, size_t bit_count)
{
	struct wide_uint maximum = wide_from_u64(0);
	struct wide_uint required;

	if (radices == NULL || digit_count == 0 ||
	    digit_count > MIXED_RADIX_MAX_DIGITS ||
	    bit_count > MIXED_RADIX_MAX_BITS)
		return false;
	required = wide_subtract(wide_shift_left(wide_from_u64(1), bit_count),
		wide_from_u64(1));
	for (size_t index = digit_count; index > 0; --index) {
		uint32_t radix = radices[index - 1];

		if (radix == 0 ||
		    !wide_multiply_u32(maximum, radix, &maximum) ||
		    !wide_add(maximum, wide_from_u64(radix - 1), &maximum))
			return false;
	}
	return wide_compare(maximum, required) >= 0;
}

static bool digits_are_in_range(
	const uint32_t *radices, const uint32_t *digits,
	size_t digit_count)
{
	for (size_t digit = 0; digit < digit_count; ++digit) {
		if (digits[digit] >= radices[digit])
			return false;
	}
	return true;
}

static bool digits_are_above_half(
	const uint32_t *radices, const uint32_t *digits,
	size_t digit_count)
{
	for (size_t index = digit_count; index > 0; --index) {
		uint32_t digit = digits[index - 1];
		uint32_t reflected = radices[index - 1] - 1 - digit;

		if (digit != reflected)
			return digit > reflected;
	}
	return false;
}

enum mixed_radix_result mixed_radix_encode(
	const uint32_t *radices, size_t digit_count,
	const uint8_t *bits, size_t bit_count,
	uint32_t *digits)
{
	uint32_t encoded[MIXED_RADIX_MAX_DIGITS];
	struct wide_uint value = wide_from_u64(0);
	uint8_t combined = 0;

	if (digits == NULL || (bits == NULL && bit_count != 0))
		return MIXED_RADIX_INVALID_ARGUMENT;
	if (!configuration_is_valid(radices, digit_count, bit_count))
		return MIXED_RADIX_INVALID_CONFIGURATION;
	/* Keep validity separate: a high non-binary bit may shift out of value. */
	for (size_t bit = bit_count; bit > 0; --bit) {
		combined |= bits[bit - 1];
		value = wide_shift_left(value, 1);
		value.low |= bits[bit - 1];
	}
	if (combined > 1)
		return MIXED_RADIX_OUT_OF_RANGE;
	for (size_t digit = 0; digit < digit_count; ++digit) {
		encoded[digit] = wide_divide_u32(value, radices[digit], &value);
	}
	if (wide_compare(value, wide_from_u64(0)) != 0)
		return MIXED_RADIX_OUT_OF_RANGE;
	memcpy(digits, encoded, digit_count * sizeof(*digits));
	return MIXED_RADIX_OK;
}

static enum mixed_radix_result decode(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *bits, size_t bit_count,
	bool reject_unused, bool validated)
{
	uint8_t decoded[MIXED_RADIX_MAX_BITS];
	struct wide_uint value = wide_from_u64(0);

	if (digits == NULL || (bits == NULL && bit_count != 0))
		return MIXED_RADIX_INVALID_ARGUMENT;
	if (!validated && !configuration_is_valid(radices, digit_count, bit_count))
		return MIXED_RADIX_INVALID_CONFIGURATION;
	if (!validated && !digits_are_in_range(radices, digits, digit_count))
		return MIXED_RADIX_OUT_OF_RANGE;
	for (size_t index = digit_count; index > 0; --index) {
		uint32_t radix = radices[index - 1];
		uint32_t digit = digits[index - 1];

		if (!wide_multiply_u32(value, radix, &value) ||
		    !wide_add(value, wide_from_u64(digit), &value))
			return MIXED_RADIX_OUT_OF_RANGE;
	}
	if (reject_unused && bit_count < MIXED_RADIX_MAX_BITS &&
	    wide_compare(value, wide_shift_left(wide_from_u64(1), bit_count)) >= 0)
		return MIXED_RADIX_OUT_OF_RANGE;
	for (size_t bit = 0; bit < bit_count; ++bit) {
		decoded[bit] = (uint8_t)(value.low & 1);
		value = wide_shift_right(value, 1);
	}
	if (bit_count != 0)
		memcpy(bits, decoded, bit_count);
	return MIXED_RADIX_OK;
}

enum mixed_radix_result mixed_radix_decode(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *bits, size_t bit_count)
{
	return decode(radices, digit_count, digits, bits, bit_count, true, false);
}

enum mixed_radix_result mixed_radix_decode_low_bits(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *bits, size_t bit_count)
{
	return decode(radices, digit_count, digits, bits, bit_count, false, false);
}

enum mixed_radix_result mixed_radix_encode_differential(
	const uint32_t *radices, size_t digit_count,
	const uint8_t *bits, size_t bit_count,
	uint8_t *differential_state, uint32_t *digits,
	uint8_t *frame_sign)
{
	uint32_t encoded[MIXED_RADIX_MAX_DIGITS];
	uint8_t previous;
	uint8_t sign;
	enum mixed_radix_result result;

	if (differential_state == NULL || digits == NULL ||
	    frame_sign == NULL)
		return MIXED_RADIX_INVALID_ARGUMENT;
	if (*differential_state > 1)
		return MIXED_RADIX_OUT_OF_RANGE;
	result = mixed_radix_encode(
		radices, digit_count, bits, bit_count, encoded);
	if (result != MIXED_RADIX_OK)
		return result;
	previous = *differential_state;
	sign = (uint8_t)(digits_are_above_half(
		radices, encoded, digit_count) ^ previous);
	if (previous != 0) {
		for (size_t digit = 0; digit < digit_count; ++digit)
			encoded[digit] = radices[digit] - 1 - encoded[digit];
	}
	memcpy(digits, encoded, digit_count * sizeof(*digits));
	*differential_state = sign;
	*frame_sign = sign;
	return MIXED_RADIX_OK;
}

enum mixed_radix_result mixed_radix_decode_differential(
	const uint32_t *radices, size_t digit_count,
	const uint32_t *digits, uint8_t *differential_state,
	uint8_t *bits, size_t bit_count)
{
	uint32_t ordinary[MIXED_RADIX_MAX_DIGITS];
	uint8_t previous;
	uint8_t next;
	enum mixed_radix_result result;

	if (digits == NULL || differential_state == NULL ||
	    (bits == NULL && bit_count != 0))
		return MIXED_RADIX_INVALID_ARGUMENT;
	if (*differential_state > 1)
		return MIXED_RADIX_OUT_OF_RANGE;
	if (!configuration_is_valid(radices, digit_count, bit_count))
		return MIXED_RADIX_INVALID_CONFIGURATION;
	if (!digits_are_in_range(radices, digits, digit_count))
		return MIXED_RADIX_OUT_OF_RANGE;
	previous = *differential_state;
	for (size_t digit = 0; digit < digit_count; ++digit)
		ordinary[digit] = previous != 0 ?
			radices[digit] - 1 - digits[digit] : digits[digit];
	next = (uint8_t)(digits_are_above_half(
		radices, ordinary, digit_count) ^ previous);
	/* Reflection needs validated digits; reuse that check for the decode. */
	result = decode(
		radices, digit_count, ordinary, bits, bit_count, true, true);
	if (result != MIXED_RADIX_OK)
		return result;
	*differential_state = next;
	return MIXED_RADIX_OK;
}
