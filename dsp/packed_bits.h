// SPDX-License-Identifier: BSD-3-Clause

#ifndef PACKED_BITS_H
#define PACKED_BITS_H

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
	for (; byte_index < capacity; ++byte_index) {
		if (bytes[byte_index] != 0)
			return false;
	}
	return true;
}

#endif
