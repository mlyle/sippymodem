// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_control_frame.h"

uint16_t pcm_control_frame_crc(
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

void pcm_control_frame_write_options(
	uint8_t *frame, uint8_t sign_redundancy, bool acknowledgment,
	enum g711_law law, uint8_t lookahead)
{
	packed_msb_write_lsb_field(frame, 31u, 2u, sign_redundancy);
	packed_msb_set_bit(frame, 33u, acknowledgment);
	packed_msb_set_bit(frame, 35u, law == G711_LAW_PCMA);
	packed_msb_write_lsb_field(frame, 49u, 2u, lookahead);
}

void pcm_control_frame_read_options(
	const uint8_t *frame, uint8_t *sign_redundancy, bool *acknowledgment,
	enum g711_law *law, uint8_t *lookahead)
{
	*sign_redundancy = (uint8_t)packed_msb_read_lsb_field(frame, 31u, 2u);
	*acknowledgment = packed_msb_bit(frame, 33u) != 0u;
	*law = packed_msb_bit(frame, 35u) != 0u ? G711_LAW_PCMA : G711_LAW_PCMU;
	*lookahead = (uint8_t)packed_msb_read_lsb_field(frame, 49u, 2u);
}

void pcm_control_frame_write_shaping(
	uint8_t *frame, uint16_t ratio, int8_t a1, int8_t a2, int8_t b1, int8_t b2)
{
	packed_msb_write_lsb_field(frame, 52u, 16u, ratio);
	packed_msb_write_lsb_field(frame, 69u, 8u, (uint8_t)a1);
	packed_msb_write_lsb_field(frame, 77u, 8u, (uint8_t)a2);
	packed_msb_write_lsb_field(frame, 86u, 8u, (uint8_t)b1);
	packed_msb_write_lsb_field(frame, 94u, 8u, (uint8_t)b2);
}

static int8_t read_q1_6(const uint8_t *frame, size_t first_bit)
{
	uint8_t raw = (uint8_t)packed_msb_read_lsb_field(frame, first_bit, 8u);

	return raw <= INT8_MAX ? (int8_t)raw :
	       (int8_t)(-1 - (int16_t)(UINT8_MAX - raw));
}

void pcm_control_frame_read_shaping(
	const uint8_t *frame, uint16_t *ratio, int8_t *a1, int8_t *a2,
	int8_t *b1, int8_t *b2)
{
	*ratio = (uint16_t)packed_msb_read_lsb_field(frame, 52u, 16u);
	*a1 = read_q1_6(frame, 69u);
	*a2 = read_q1_6(frame, 77u);
	*b1 = read_q1_6(frame, 86u);
	*b2 = read_q1_6(frame, 94u);
}

/* CP, CPt and CPu share 136-bit constellation rows after a 136-bit header.
 * Each row carries eight words: a start bit followed by sixteen Ucode bits,
 * lowest Ucode first. The caller validates row counts and storage bounds.
 * Only mask information is read/written; framing and padding stay with the
 * record owner. Decoding replaces each mask without requiring zeroed rows. */
static void write_constellations(
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

static void read_constellations(
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

void pcm_control_frame_write_constellation_payload(
	uint8_t *frame, const struct pcm_constellation *transmit,
	const struct pcm_constellation *codec, size_t count, bool codec_differs)
{
	size_t payload_end = pcm_control_frame_payload_end(count, codec_differs);

	write_constellations(frame, 0u, transmit, count);
	if (codec_differs)
		write_constellations(frame, count, codec, count);
	packed_msb_write_msb_field(frame, payload_end + 1u, 16u,
		pcm_control_frame_crc(frame, payload_end));
}

bool pcm_control_frame_read_constellation_payload(
	const uint8_t *frame, struct pcm_constellation *transmit,
	struct pcm_constellation *codec, size_t count, bool codec_differs)
{
	size_t payload_end = pcm_control_frame_payload_end(count, codec_differs);
	uint16_t received = (uint16_t)packed_msb_read_msb_field(
		frame, payload_end + 1u, 16u);

	if (received != pcm_control_frame_crc(frame, payload_end))
		return false;
	read_constellations(frame, 0u, transmit, count);
	if (codec_differs)
		read_constellations(frame, count, codec, count);
	return true;
}
