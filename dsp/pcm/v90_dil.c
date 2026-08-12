// SPDX-License-Identifier: BSD-3-Clause

#include "v90_dil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	START_FIRST_BIT = 17,
	INFORMATION_FIRST_BIT = 18,
	N_FIRST_BIT = 18,
	N_BITS = 8,
	LSP_MINUS_ONE_FIRST_BIT = 35,
	LTP_MINUS_ONE_FIRST_BIT = 43,
	LENGTH_BITS = 7,
	CRC_START_BIT = 221,
	CRC_FIRST_BIT = 222,
	CRC_BITS = 16,
	FILL_FIRST_BIT = 238,
	FIELD_STRIDE_BITS = 17,
	CRC_INITIAL_REMAINDER = 0xffff,
	CRC_REFLECTED_POLYNOMIAL = 0x8408,
};

static uint8_t descriptor_bit(
	const uint8_t *descriptor, size_t index)
{
	return (uint8_t)((descriptor[index / 8u] >>
			  (7u - index % 8u)) &
			 1u);
}

static void set_descriptor_bit(
	uint8_t *descriptor, size_t index, bool value)
{
	if (value)
		descriptor[index / 8u] |=
			(uint8_t)(1u << (7u - index % 8u));
}

static bool is_start_bit(size_t index)
{
	return index >= START_FIRST_BIT && index <= CRC_START_BIT &&
	       (index - START_FIRST_BIT) % FIELD_STRIDE_BITS == 0;
}

static uint16_t crc_step(uint16_t remainder, bool bit)
{
	uint16_t next = (uint16_t)(remainder >> 1);

	if (((remainder ^ (uint16_t)bit) & 1u) != 0)
		next ^= CRC_REFLECTED_POLYNOMIAL;
	return next;
}

static uint16_t descriptor_crc(const uint8_t *descriptor)
{
	uint16_t remainder = CRC_INITIAL_REMAINDER;

	for (size_t index = INFORMATION_FIRST_BIT;
	     index < CRC_START_BIT; ++index) {
		if (!is_start_bit(index))
			remainder = crc_step(
				remainder, descriptor_bit(descriptor, index) != 0);
	}
	return remainder;
}

static uint16_t encoded_crc(const uint8_t *descriptor)
{
	uint16_t value = 0;

	for (size_t index = 0; index < CRC_BITS; ++index)
		value |= (uint16_t)descriptor_bit(
				 descriptor, CRC_FIRST_BIT + index) << index;
	return value;
}

static bool header_is_valid(const uint8_t *descriptor)
{
	for (size_t index = 0; index < V90_DIL_FRAME_SYNC_BITS; ++index) {
		if (descriptor_bit(descriptor, index) == 0)
			return false;
	}
	return descriptor_bit(descriptor, START_FIRST_BIT) == 0 &&
	       descriptor_bit(
		       descriptor, START_FIRST_BIT + FIELD_STRIDE_BITS) == 0;
}

static bool zero_framing_is_valid(const uint8_t *descriptor)
{
	for (size_t index = START_FIRST_BIT;
	     index <= CRC_START_BIT; index += FIELD_STRIDE_BITS) {
		if (descriptor_bit(descriptor, index) != 0)
			return false;
	}
	return descriptor_bit(descriptor, FILL_FIRST_BIT) == 0 &&
	       descriptor_bit(descriptor, FILL_FIRST_BIT + 1u) == 0;
}

static uint8_t descriptor_n(const uint8_t *descriptor)
{
	uint8_t value = 0;

	for (size_t index = 0; index < N_BITS; ++index)
		value |= (uint8_t)(descriptor_bit(
				 descriptor, N_FIRST_BIT + index) << index);
	return value;
}

static bool zero_lengths_are_valid(const uint8_t *descriptor)
{
	for (size_t index = 0; index < LENGTH_BITS; ++index) {
		if (descriptor_bit(
			    descriptor, LSP_MINUS_ONE_FIRST_BIT + index) != 0 ||
		    descriptor_bit(
			    descriptor, LTP_MINUS_ONE_FIRST_BIT + index) != 0)
			return false;
	}
	return true;
}

enum v90_dil_result v90_dil_encode_zero(
	uint8_t *descriptor, size_t descriptor_capacity)
{
	uint8_t encoded[V90_DIL_ZERO_DESCRIPTOR_BYTES] = { 0 };
	uint16_t remainder;

	if (descriptor == NULL || descriptor_capacity < sizeof(encoded))
		return V90_DIL_INVALID_ARGUMENT;
	for (size_t index = 0; index < V90_DIL_FRAME_SYNC_BITS; ++index)
		set_descriptor_bit(encoded, index, true);
	remainder = descriptor_crc(encoded);
	for (size_t index = 0; index < CRC_BITS; ++index)
		set_descriptor_bit(
			encoded, CRC_FIRST_BIT + index,
			(remainder & (uint16_t)(1u << index)) != 0);
	memcpy(descriptor, encoded, sizeof(encoded));
	return V90_DIL_OK;
}

enum v90_dil_result v90_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity)
{
	if (descriptor == NULL ||
	    descriptor_capacity < V90_DIL_ZERO_DESCRIPTOR_BYTES)
		return V90_DIL_INVALID_ARGUMENT;
	if (!header_is_valid(descriptor))
		return V90_DIL_INVALID_FRAME;
	if (descriptor_n(descriptor) != 0)
		return V90_DIL_NONZERO_DESCRIPTOR;
	if (!zero_lengths_are_valid(descriptor) ||
	    !zero_framing_is_valid(descriptor))
		return V90_DIL_INVALID_FRAME;
	if (descriptor_crc(descriptor) != encoded_crc(descriptor))
		return V90_DIL_CRC_MISMATCH;
	return V90_DIL_OK;
}

const char *v90_dil_result_name(enum v90_dil_result result)
{
	switch (result) {
	case V90_DIL_OK:
		return "ok";
	case V90_DIL_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DIL_INVALID_FRAME:
		return "invalid frame";
	case V90_DIL_CRC_MISMATCH:
		return "CRC mismatch";
	case V90_DIL_NONZERO_DESCRIPTOR:
		return "nonzero descriptor";
	}
	return "unknown";
}
