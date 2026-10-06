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

/* J, MP, CP and DIL records carry 16 information bits after each zero start
 * bit and use the V.34 10.1.2.3.2 CRC generator. Exclude frame sync, start
 * and fill bits. The unreflected numeric representation holds the first CRC
 * output in its most-significant bit, including for LSB-first DIL fields.
 * The caller validates storage and supplies the exclusive payload end; this
 * may point at the CRC word's start bit or its first information bit. */
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

/* V.90 Table 14 and V.92 Table 23 share these Q3.13/Q1.6 fields.
 * Callers provide thirteen header bytes. Start and unrelated bits remain
 * unchanged; record owners validate storage and coefficient semantics. */
static inline void pcm_control_frame_write_shaping(
	uint8_t *frame, uint16_t ratio, int8_t a1, int8_t a2,
	int8_t b1, int8_t b2)
{
	packed_msb_write_lsb_field(frame, 52u, 16u, ratio);
	packed_msb_write_lsb_field(frame, 69u, 8u, (uint8_t)a1);
	packed_msb_write_lsb_field(frame, 77u, 8u, (uint8_t)a2);
	packed_msb_write_lsb_field(frame, 86u, 8u, (uint8_t)b1);
	packed_msb_write_lsb_field(frame, 94u, 8u, (uint8_t)b2);
}

/* Read one signed Q1.6 coefficient as eight LSB-first bits. The caller
 * supplies its bounded first bit; return the exact transmitted integer. */
static inline int8_t pcm_control_frame_read_q1_6(
	const uint8_t *frame, size_t first_bit)
{
	uint8_t raw = (uint8_t)packed_msb_read_lsb_field(frame, first_bit, 8u);

	return raw <= INT8_MAX ? (int8_t)raw :
	       (int8_t)(-1 - (int16_t)(UINT8_MAX - raw));
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

/* CP, CPt and CPu have the same 136-bit header, constellation rows and
 * 16-bit CRC word. Record owners validate the row count and frame bounds;
 * their distinct header, framing and tail rules stay protocol-local. */
static inline void pcm_control_frame_write_constellation_payload(
	uint8_t *frame, const struct pcm_constellation *transmit,
	const struct pcm_constellation *codec, size_t count, bool codec_differs)
{
	size_t row_count = count * (codec_differs ? 2u : 1u);
	size_t payload_end = (1u + row_count) * 136u;

	pcm_control_frame_write_constellations(frame, 0u, transmit, count);
	if (codec_differs)
		pcm_control_frame_write_constellations(frame, count, codec, count);
	packed_msb_write_msb_field(frame, payload_end + 1u, 16u,
		pcm_control_frame_crc(frame, payload_end));
}

/* Verify the shared CRC before changing either output constellation plane. */
static inline bool pcm_control_frame_read_constellation_payload(
	const uint8_t *frame, struct pcm_constellation *transmit,
	struct pcm_constellation *codec, size_t count, bool codec_differs)
{
	size_t row_count = count * (codec_differs ? 2u : 1u);
	size_t payload_end = (1u + row_count) * 136u;
	uint16_t received = (uint16_t)packed_msb_read_msb_field(
		frame, payload_end + 1u, 16u);

	if (received != pcm_control_frame_crc(frame, payload_end))
		return false;
	pcm_control_frame_read_constellations(frame, 0u, transmit, count);
	if (codec_differs)
		pcm_control_frame_read_constellations(frame, count, codec, count);
	return true;
}

#endif /* PCM_CONTROL_FRAME_H */
