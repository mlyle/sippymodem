// SPDX-License-Identifier: BSD-3-Clause

#ifndef PACKED_BITS_H
#define PACKED_BITS_H

#include "memory_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bit zero is the most-significant bit of byte zero. */
static inline uint8_t packed_msb_bit(
	const uint8_t *bytes, size_t bit_index)
{
	return (uint8_t)((bytes[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static inline bool packed_msb_bits_are_zero(
	const uint8_t *bytes, size_t first_bit, size_t bit_count)
{
	if (bytes == NULL)
		return false;
	for (size_t bit = 0; bit < bit_count; ++bit) {
		if (packed_msb_bit(bytes, first_bit + bit) != 0u)
			return false;
	}
	return true;
}

/* Assign one bit using the same most-significant-bit-first numbering. */
static inline void packed_msb_set_bit(
	uint8_t *bytes, size_t bit_index, bool value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (value)
		bytes[bit_index / 8u] |= mask;
	else
		bytes[bit_index / 8u] &= (uint8_t)~mask;
}

/* Read or write a field whose first transmitted bit is the value's least
 * significant bit. Storage bit numbering remains most-significant-bit first.
 * The caller supplies valid storage and a field width no greater than 32. */
static inline uint32_t packed_msb_read_lsb_field(
	const uint8_t *bytes, size_t first_bit, size_t bit_count)
{
	uint32_t value = 0;

	for (size_t bit = 0; bit < bit_count; ++bit) {
		value |= (uint32_t)packed_msb_bit(bytes, first_bit + bit) << bit;
	}
	return value;
}

static inline void packed_msb_write_lsb_field(
	uint8_t *bytes, size_t first_bit, size_t bit_count, uint32_t value)
{
	for (size_t bit = 0; bit < bit_count; ++bit) {
		packed_msb_set_bit(
			bytes, first_bit + bit,
			(value & (UINT32_C(1) << bit)) != 0);
	}
}

/* Read or write the conventional most-significant-bit-first field form. The
 * same storage and width preconditions as the LSB-field helpers apply. */
static inline uint32_t packed_msb_read_msb_field(
	const uint8_t *bytes, size_t first_bit, size_t bit_count)
{
	uint32_t value = 0;

	for (size_t bit = 0; bit < bit_count; ++bit) {
		value = (value << 1) |
			packed_msb_bit(bytes, first_bit + bit);
	}
	return value;
}

static inline void packed_msb_write_msb_field(
	uint8_t *bytes, size_t first_bit, size_t bit_count, uint32_t value)
{
	for (size_t bit = 0; bit < bit_count; ++bit) {
		packed_msb_set_bit(
			bytes, first_bit + bit,
			(value & (UINT32_C(1) <<
				  (bit_count - bit - 1u))) != 0);
	}
}

/* Unpacked bits occupy one octet each and must be exactly zero or one. */
static inline bool unpacked_bits_are_binary(
	const uint8_t *bits, size_t count)
{
	if (bits == NULL)
		return false;
	for (size_t index = 0; index < count; ++index) {
		if (bits[index] > 1u)
			return false;
	}
	return true;
}

/* Return true when every bit after used_bit_count is zero. */
static inline bool packed_msb_tail_is_zero(
	const uint8_t *bytes, size_t used_bit_count, size_t capacity)
{
	size_t byte_index = used_bit_count / 8u;
	size_t used_bits = used_bit_count % 8u;

	if (bytes == NULL || byte_index > capacity ||
	    (byte_index == capacity && used_bits != 0))
		return false;
	if (used_bits != 0) {
		uint8_t unused_mask = (uint8_t)(UINT8_MAX >> used_bits);

		if ((bytes[byte_index] & unused_mask) != 0)
			return false;
		++byte_index;
	}
	return memory_region_is_zero(
		bytes + byte_index, capacity - byte_index);
}

#endif
