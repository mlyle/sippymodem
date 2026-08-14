// SPDX-License-Identifier: BSD-3-Clause

#ifndef PACKED_BITS_H
#define PACKED_BITS_H

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

#endif
