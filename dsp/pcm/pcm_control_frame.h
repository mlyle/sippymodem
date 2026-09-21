// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONTROL_FRAME_H
#define PCM_CONTROL_FRAME_H

#include "crc16_ccitt_false.h"
#include "packed_bits.h"

/* The caller owns storage bounds. A complete prefix needs three bytes:
 * seventeen one bits followed by the first zero start bit, MSB first. */
static inline bool pcm_control_frame_prefix_is_valid(const uint8_t *frame)
{
	return frame[0] == UINT8_MAX && frame[1] == UINT8_MAX &&
	       (frame[2] & 0xc0u) == 0x80u;
}

/* Retain a rejected record's possible sync suffix, capped at the prefix's
 * seventeen ones. Bits at or after bit_count are not part of the record. */
static inline uint8_t pcm_control_frame_trailing_ones(
	const uint8_t *frame, size_t bit_count)
{
	uint8_t count = 0;

	while (bit_count != 0 && count < 17u) {
		if (packed_msb_bit(frame, bit_count - 1u) == 0u)
			break;
		count++;
		bit_count--;
	}
	return count;
}

/* MP and CP-family records carry 16 information bits after each zero start
 * bit. Exclude the 17-bit sync prefix and start bits from the unreflected CRC.
 * The caller validates storage and supplies the exclusive payload end; this
 * may point at the CRC word's start bit or its first information bit. DIL
 * descriptors use a different, reflected CRC and do not use this helper. */
static inline uint16_t pcm_control_frame_crc(
	const uint8_t *frame, size_t payload_end)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t bit = 18u; bit < payload_end; ++bit) {
		if (bit % 17u != 0u)
			remainder = crc16_ccitt_false_step(
				remainder, packed_msb_bit(frame, bit) != 0u);
	}
	return remainder;
}

#endif /* PCM_CONTROL_FRAME_H */
