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
