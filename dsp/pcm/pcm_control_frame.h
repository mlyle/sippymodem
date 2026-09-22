// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONTROL_FRAME_H
#define PCM_CONTROL_FRAME_H

#include "crc16_ccitt_false.h"
#include "packed_bits.h"
#include "pcm_constellation.h"

/* Write the seventeen sync ones and first zero start bit into three bytes.
 * The caller owns storage bounds; information bits 18 onward are preserved. */
static inline void pcm_control_frame_write_prefix(uint8_t *frame)
{
	frame[0] = UINT8_MAX;
	frame[1] = UINT8_MAX;
	frame[2] = (uint8_t)((frame[2] & 0x3fu) | 0x80u);
}

/* The caller owns storage bounds. A complete prefix needs three bytes:
 * seventeen one bits followed by the first zero start bit, MSB first. */
static inline bool pcm_control_frame_prefix_is_valid(const uint8_t *frame)
{
	return frame[0] == UINT8_MAX && frame[1] == UINT8_MAX &&
	       (frame[2] & 0xc0u) == 0x80u;
}

/* Consume one bit, reporting a prefix only at the zero after at least
 * seventeen ones. The receiver owns and validates the saturated 0..17 count,
 * including any retained suffix of a rejected record. Every zero clears it. */
static inline bool pcm_control_frame_sync_bit(uint8_t *ones, bool bit)
{
	if (bit) {
		if (*ones < 17u)
			++*ones;
		return false;
	}
	bool complete = *ones == 17u;

	*ones = 0;
	return complete;
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

/* Check only the zero start bits of consecutive 17-bit control words.
 * The caller supplies a start-bit position and a bounded exclusive end;
 * a partial final word still has its start bit checked. Information and
 * bits outside this range are deliberately ignored. */
static inline bool pcm_control_frame_starts_are_zero(
	const uint8_t *frame, size_t first_start, size_t end)
{
	for (size_t bit = first_start; bit < end; bit += 17u)
		if (packed_msb_bit(frame, bit) != 0u)
			return false;
	return true;
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

/* CP, CPt and CPu map six frame intervals to constellation rows. Each index
 * is four bits, LSB first; the start bit at 119 splits the first four indices
 * from the last two. Callers provide at least sixteen header bytes and six
 * map entries. Writing requires four-bit indices; reading rejects indices
 * beyond the six supported rows. Framing bits remain with the record owner. */
static inline size_t pcm_control_frame_map_bit(size_t interval)
{
	return 103u + interval * 4u + (interval >= 4u ? 1u : 0u);
}

static inline void pcm_control_frame_write_map(
	uint8_t *frame, const uint8_t map[PCM_CONSTELLATION_FRAME_INTERVALS])
{
	for (size_t interval = 0;
	     interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval)
		packed_msb_write_lsb_field(frame,
			pcm_control_frame_map_bit(interval), 4u, map[interval]);
}

/* Return the declared row count, or zero for an out-of-range index. A NULL
 * map probes the shape without copying indices. Otherwise map may be partly
 * filled on failure; record owners retain their transactional output policy.
 * Missing rows are not rejected here: CP cleardown ignores their contents. */
static inline size_t pcm_control_frame_read_map(
	const uint8_t *frame, uint8_t *map)
{
	size_t count = 1u;

	for (size_t interval = 0;
	     interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
		uint8_t index = (uint8_t)packed_msb_read_lsb_field(
			frame, pcm_control_frame_map_bit(interval), 4u);

		if (index >= PCM_CONSTELLATION_MAX_ROWS)
			return 0;
		if (map != NULL)
			map[interval] = index;
		if ((size_t)index + 1u > count)
			count = (size_t)index + 1u;
	}
	return count;
}

/* CP, CPt and CPu share 136-bit constellation rows after a 136-bit header.
 * Each row carries eight words: a start bit followed by sixteen Ucode bits,
 * lowest Ucode first. The caller validates row counts and storage bounds.
 * Only mask information is read/written; framing and padding stay with the
 * record owner. Decoding replaces each mask without requiring zeroed rows. */
static inline void pcm_control_frame_write_constellations(
	uint8_t *frame, size_t first_row,
	const struct pcm_constellation *rows, size_t count)
{
	for (size_t row = 0; row < count; ++row) {
		size_t first = (first_row + row + 1u) * 136u + 1u;

		for (size_t byte = 0; byte < PCM_CONSTELLATION_MASK_BYTES; ++byte)
			packed_msb_write_lsb_field(frame,
				first + byte * 8u + byte / 2u, 8u,
				rows[row].ucode_mask[byte]);
	}
}

static inline void pcm_control_frame_read_constellations(
	const uint8_t *frame, size_t first_row,
	struct pcm_constellation *rows, size_t count)
{
	for (size_t row = 0; row < count; ++row) {
		size_t first = (first_row + row + 1u) * 136u + 1u;

		for (size_t byte = 0; byte < PCM_CONSTELLATION_MASK_BYTES; ++byte)
			rows[row].ucode_mask[byte] =
				(uint8_t)packed_msb_read_lsb_field(frame,
					first + byte * 8u + byte / 2u, 8u);
	}
}

#endif /* PCM_CONTROL_FRAME_H */
