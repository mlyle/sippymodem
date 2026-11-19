// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_j_frame.h"

#include "packed_bits.h"
#include "pcm_control_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	FIRST_PAYLOAD_BIT = 18,
	FIRST_PAYLOAD_BITS = 16,
	SECOND_PAYLOAD_BIT = 35,
	SECOND_PAYLOAD_BITS = 16,
	CRC_START_BIT = 51,
};

static void encode_payload(uint8_t *frame, uint32_t payload)
{
	packed_msb_write_lsb_field(
		frame, FIRST_PAYLOAD_BIT, FIRST_PAYLOAD_BITS, payload);
	packed_msb_write_lsb_field(
		frame, SECOND_PAYLOAD_BIT, SECOND_PAYLOAD_BITS,
		payload >> FIRST_PAYLOAD_BITS);
}

enum pcm_j_frame_result pcm_j_frame_encode(
	uint32_t payload, uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[PCM_J_FRAME_BYTES] = { 0 };

	if (frame == NULL || frame_capacity < sizeof(encoded))
		return PCM_J_FRAME_INVALID_ARGUMENT;
	pcm_control_frame_write_prefix(encoded);
	encode_payload(encoded, payload);
	pcm_control_frame_write_crc(encoded, CRC_START_BIT);
	memcpy(frame, encoded, sizeof(encoded));
	return PCM_J_FRAME_OK;
}

static uint32_t decode_payload(const uint8_t *frame)
{
	return packed_msb_read_lsb_field(
		       frame, FIRST_PAYLOAD_BIT, FIRST_PAYLOAD_BITS) |
	       packed_msb_read_lsb_field(
		       frame, SECOND_PAYLOAD_BIT, SECOND_PAYLOAD_BITS)
		       << FIRST_PAYLOAD_BITS;
}

enum pcm_j_frame_result pcm_j_frame_decode(
	const uint8_t *frame, size_t frame_capacity, uint32_t *payload)
{
	uint32_t decoded;

	if (frame == NULL || payload == NULL ||
	    frame_capacity < PCM_J_FRAME_BYTES)
		return PCM_J_FRAME_INVALID_ARGUMENT;
	if (!pcm_control_frame_wire_is_valid(frame, CRC_START_BIT, PCM_J_FRAME_BITS))
		return PCM_J_FRAME_INVALID_FRAME;
	decoded = decode_payload(frame);
	if (!pcm_control_frame_crc_matches(frame, CRC_START_BIT))
		return PCM_J_FRAME_CRC_MISMATCH;
	*payload = decoded;
	return PCM_J_FRAME_OK;
}
