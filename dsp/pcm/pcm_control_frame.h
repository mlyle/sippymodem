// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONTROL_FRAME_H
#define PCM_CONTROL_FRAME_H

#include "crc16_ccitt_false.h"
#include "g711.h"
#include "packed_bits.h"
#include "pcm_constellation.h"

enum {
	PCM_CONTROL_CONSTELLATION_ROW_BITS = 136,
	PCM_CONTROL_CODEC_DIFFERENCE_BIT = 128,
	PCM_CONTROL_CONSTELLATION_TRAILER_BITS = 18,
};

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

/* The caller bounds the CRC word and exclusive frame end. */
static inline bool pcm_control_frame_crc_fill_is_zero(
	const uint8_t *frame, size_t crc_start, size_t frame_bits)
{
	return packed_msb_bits_are_zero(frame, crc_start + 17u,
		frame_bits - crc_start - 17u);
}

/* These control records share prefix, word starts and zero fill after the CRC.
 * The caller validates storage and exact wire length; payload_end is the
 * CRC word's zero start bit and its 16 information bits must follow.
 * Bits outside frame_bits and protocol fields stay with the codec. */
static inline bool pcm_control_frame_wire_is_valid(
	const uint8_t *frame, size_t payload_end, size_t frame_bits)
{
	return pcm_control_frame_prefix_is_valid(frame) &&
	       pcm_control_frame_starts_are_zero(frame, 34u, payload_end + 1u) &&
	       pcm_control_frame_crc_fill_is_zero(frame, payload_end, frame_bits);
}

/* Use this when the record also requires zero unused bits in its final storage
 * byte. MP checks only the wire bits with pcm_control_frame_wire_is_valid. */
static inline bool pcm_control_frame_framing_is_valid(
	const uint8_t *frame, size_t payload_end, size_t frame_bits)
{
	return pcm_control_frame_wire_is_valid(frame, payload_end, frame_bits) &&
	       packed_msb_bits_are_zero(frame, frame_bits,
		       (8u - frame_bits % 8u) % 8u);
}

/* J, MP, CP and DIL records carry 16 information bits after each zero start
 * bit and use the V.34 10.1.2.3.2 CRC generator. Exclude frame sync, start
 * and fill bits. The unreflected numeric representation holds the first CRC
 * output in its most-significant bit, including for LSB-first DIL fields.
 * The caller validates storage and supplies the exclusive payload end; this
 * may point at the CRC word's start bit or its first information bit. */
uint16_t pcm_control_frame_crc(
	const uint8_t *frame, size_t payload_end);

/* Read or write the sixteen CRC bits following their start bit. Record
 * owners verify bounds, start bits and any fill before checking the CRC. */
void pcm_control_frame_write_crc(uint8_t *frame, size_t crc_start);
bool pcm_control_frame_crc_matches(const uint8_t *frame, size_t crc_start);

/* CP, CPt and CPu share these Table 14/V.90 and Table 23/V.92 slots.
 * Callers provide seven header bytes and validated field values. Record
 * owners interpret the acknowledgment and retain their rate/type fields;
 * start and unrelated bits remain unchanged. */
void pcm_control_frame_write_options(
	uint8_t *frame, uint8_t sign_redundancy, bool acknowledgment,
	enum g711_law law, uint8_t lookahead);

/* Read the same slots into separately owned fields. The caller validates
 * framing and storage; no protocol-specific interpretation is applied. */
void pcm_control_frame_read_options(
	const uint8_t *frame, uint8_t *sign_redundancy, bool *acknowledgment,
	enum g711_law *law, uint8_t *lookahead);

/* V.90 Table 14 and V.92 Table 23 share these Q3.13/Q1.6 fields.
 * Callers provide thirteen header bytes. Start and unrelated bits remain
 * unchanged; record owners validate storage and coefficient semantics. */
void pcm_control_frame_write_shaping(
	uint8_t *frame, uint16_t ratio, int8_t a1, int8_t a2, int8_t b1, int8_t b2);

/* Read the matching Table 14/V.90 and Table 23/V.92 shaping fields.
 * Callers provide thirteen header bytes and separately owned output fields;
 * record owners retain validation of framing and coefficient semantics. */
void pcm_control_frame_read_shaping(
	const uint8_t *frame, uint16_t *ratio, int8_t *a1, int8_t *a2,
	int8_t *b1, int8_t *b2);

/* CP, CPt and CPu map six frame intervals to constellation rows. Each index
 * is four bits, LSB first; the start bit at 119 splits the first four indices
 * from the last two. Callers provide at least seventeen header bytes and
 * six map entries. The plane flag is at bit 128.
 * Writing requires four-bit indices; reading accepts up to six rows. */
static inline size_t pcm_control_frame_map_bit(size_t interval)
{
	return 103u + interval * 4u + (interval >= 4u ? 1u : 0u);
}

static inline void pcm_control_frame_write_map(
	uint8_t *frame, const uint8_t map[PCM_CONSTELLATION_FRAME_INTERVALS], bool codec_differs)
{
	for (size_t interval = 0;
	     interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval)
		packed_msb_write_lsb_field(frame,
			pcm_control_frame_map_bit(interval), 4u, map[interval]);
	packed_msb_set_bit(frame, PCM_CONTROL_CODEC_DIFFERENCE_BIT, codec_differs);
}

struct pcm_control_constellation_layout {
	uint8_t rows;
	bool codec_differs;
};

/* The map and plane flag are complete at bit 129. A NULL map probes the
 * layout without copying indices; zero rows reports an invalid index.
 * Record owners interpret unused rows and publish decoded fields. */
static inline struct pcm_control_constellation_layout
pcm_control_frame_read_constellation_layout(const uint8_t *frame, uint8_t *map)
{
	uint8_t count = 1u;

	for (size_t interval = 0;
	     interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
		uint8_t index = (uint8_t)packed_msb_read_lsb_field(
			frame, pcm_control_frame_map_bit(interval), 4u);

		if (index >= PCM_CONSTELLATION_MAX_ROWS)
			return (struct pcm_control_constellation_layout){ 0 };
		if (map != NULL)
			map[interval] = index;
		if (index + 1u > count)
			count = index + 1u;
	}
	return (struct pcm_control_constellation_layout){
		.rows = count,
		.codec_differs = packed_msb_bit(frame, PCM_CONTROL_CODEC_DIFFERENCE_BIT) != 0u,
	};
}

/* Table 14/V.90 and Table 23/V.92 share one 136-bit header followed by
 * one or two planes of 136-bit constellation rows. The caller validates
 * the declared row count (1..6) before calculating the CRC start bit. */
static inline size_t pcm_control_frame_payload_end(size_t count, bool codec_differs)
{
	return (1u + count * (codec_differs ? 2u : 1u)) *
		PCM_CONTROL_CONSTELLATION_ROW_BITS;
}

/* Round a bounded control record, including its mandatory trailer, to the
 * caller's positive frame-bit alignment. Record owners select the trailer
 * and alignment for their protocol and preceding modulation. */
static inline size_t pcm_control_frame_align_bits(size_t bits, size_t alignment)
{
	return (bits + alignment - 1u) / alignment * alignment;
}

/* The CRC word and first zero fill bit occupy eighteen bits. CP aligns to
 * four bits, CPt to twelve, and CPu to the negotiated mapping frame. */
static inline size_t pcm_control_frame_constellation_bits(
	size_t rows, bool codec_differs, size_t alignment)
{
	return rows == 0 ? 0 :
		pcm_control_frame_align_bits(
			pcm_control_frame_payload_end(rows, codec_differs) +
			PCM_CONTROL_CONSTELLATION_TRAILER_BITS, alignment);
}

/* CP, CPt and CPu have the same 136-bit header, constellation rows and
 * 16-bit CRC word. Record owners validate the row count and frame bounds;
 * their distinct header, framing and tail rules stay protocol-local. */
void pcm_control_frame_write_constellation_payload(
	uint8_t *frame, const struct pcm_constellation *transmit,
	const struct pcm_constellation *codec, size_t count, bool codec_differs);

/* Verify the shared CRC before changing either output constellation plane. */
bool pcm_control_frame_read_constellation_payload(
	const uint8_t *frame, struct pcm_constellation *transmit,
	struct pcm_constellation *codec, size_t count, bool codec_differs);

enum pcm_control_constellation_result {
	PCM_CONTROL_CONSTELLATION_OK,
	PCM_CONTROL_CONSTELLATION_INVALID_FRAME,
	PCM_CONTROL_CONSTELLATION_CRC_MISMATCH,
};

/* Check the shared CP/CPt/CPu length, starts, fill and CRC before reading
 * the rows. Callers validate storage, fixed type bits and decoded fields. */
enum pcm_control_constellation_result pcm_control_frame_decode_constellations(
	const uint8_t *frame, size_t frame_bits, size_t alignment,
	struct pcm_control_constellation_layout layout,
	struct pcm_constellation *transmit, struct pcm_constellation *codec);

#endif /* PCM_CONTROL_FRAME_H */
