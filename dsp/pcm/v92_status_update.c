// SPDX-License-Identifier: BSD-3-Clause

#include "v92_status_update.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <stdbool.h>
#include <string.h>

enum {
	STATUS_MARKER_BIT = 18,
	STATUS_PAYLOAD_FIRST_BIT = 19,
	STATUS_CRC_START_BIT = 34,
	STATUS_CRC_FIRST_BIT = 35,
	STATUS_CRC_BITS = 16,
	STATUS_FILL_FIRST_BIT = 51,
};

static bool alignment_is_valid(unsigned int alignment_bits)
{
	return (alignment_bits >=
			V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS &&
		alignment_bits <=
			V92_STATUS_UPDATE_DOWNSTREAM_DATA_MAX_BITS) ||
	       v92_control_upstream_alignment_is_valid(alignment_bits);
}

size_t v92_status_update_frame_bits(unsigned int alignment_bits)
{
	size_t unfilled = V92_STATUS_UPDATE_INFORMATION_BITS +
		V92_STATUS_UPDATE_TRAILER_BITS;

	if (!alignment_is_valid(alignment_bits))
		return 0u;
	return (unfilled + alignment_bits - 1u) / alignment_bits *
		alignment_bits;
}

size_t v92_status_update_frame_bytes(unsigned int alignment_bits)
{
	size_t bits = v92_status_update_frame_bits(alignment_bits);

	return bits == 0u ? 0u : (bits + 7u) / 8u;
}

enum v92_status_update_result v92_status_update_encode(
	uint16_t payload, unsigned int alignment_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	uint8_t encoded[V92_STATUS_UPDATE_MAX_FRAME_BYTES] = { 0 };
	size_t bit_count;
	size_t byte_count;

	if (frame == NULL || frame_bit_count == NULL)
		return V92_STATUS_UPDATE_INVALID_ARGUMENT;
	if ((payload >> V92_STATUS_UPDATE_PAYLOAD_BITS) != 0u)
		return V92_STATUS_UPDATE_INVALID_ARGUMENT;
	bit_count = v92_status_update_frame_bits(alignment_bits);
	if (bit_count == 0u)
		return V92_STATUS_UPDATE_INVALID_ALIGNMENT;
	byte_count = (bit_count + 7u) / 8u;
	if (frame_capacity < byte_count)
		return V92_STATUS_UPDATE_OUTPUT_TOO_SMALL;

	for (size_t bit = 0u; bit < 17u; ++bit)
		packed_msb_set_bit(encoded, bit, true);
	packed_msb_set_bit(encoded, STATUS_MARKER_BIT, true);
	packed_msb_write_lsb_field(
		encoded, STATUS_PAYLOAD_FIRST_BIT,
		V92_STATUS_UPDATE_PAYLOAD_BITS, payload);
	packed_msb_write_msb_field(
		encoded, STATUS_CRC_FIRST_BIT, STATUS_CRC_BITS,
		pcm_control_frame_crc(encoded, STATUS_CRC_START_BIT));
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V92_STATUS_UPDATE_OK;
}

enum v92_status_update_result v92_status_update_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int alignment_bits,
	uint16_t *payload)
{
	size_t expected_bits;
	size_t expected_bytes;
	uint16_t decoded;

	if (frame == NULL || payload == NULL)
		return V92_STATUS_UPDATE_INVALID_ARGUMENT;
	expected_bits = v92_status_update_frame_bits(alignment_bits);
	if (expected_bits == 0u)
		return V92_STATUS_UPDATE_INVALID_ALIGNMENT;
	if (frame_bit_count != expected_bits)
		return V92_STATUS_UPDATE_INVALID_FRAME;
	expected_bytes = (expected_bits + 7u) / 8u;
	if (frame_capacity < expected_bytes)
		return V92_STATUS_UPDATE_INVALID_ARGUMENT;
	if (!pcm_control_frame_prefix_is_valid(frame) ||
	    packed_msb_bit(frame, STATUS_MARKER_BIT) != 1u ||
	    packed_msb_bit(frame, STATUS_CRC_START_BIT) != 0u ||
	    !packed_msb_bits_are_zero(
		    frame, STATUS_FILL_FIRST_BIT,
		    frame_bit_count - STATUS_FILL_FIRST_BIT) ||
	    !packed_msb_tail_is_zero(
		    frame, frame_bit_count, expected_bytes))
		return V92_STATUS_UPDATE_INVALID_FRAME;
	if (packed_msb_read_msb_field(
		    frame, STATUS_CRC_FIRST_BIT, STATUS_CRC_BITS) !=
	    pcm_control_frame_crc(frame, STATUS_CRC_START_BIT))
		return V92_STATUS_UPDATE_CRC_MISMATCH;
	decoded = (uint16_t)packed_msb_read_lsb_field(
		frame, STATUS_PAYLOAD_FIRST_BIT,
		V92_STATUS_UPDATE_PAYLOAD_BITS);
	*payload = decoded;
	return V92_STATUS_UPDATE_OK;
}
