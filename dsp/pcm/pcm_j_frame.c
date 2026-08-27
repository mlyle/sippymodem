// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_j_frame.h"

#include "crc16_ccitt_false.h"
#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	FRAME_SYNC_FIRST_BIT = 0,
	START0_BIT = 17,
	FIRST_PAYLOAD_BIT = 18,
	FIRST_PAYLOAD_BITS = 16,
	START1_BIT = 34,
	SECOND_PAYLOAD_BIT = 35,
	SECOND_PAYLOAD_BITS = 16,
	START2_BIT = 51,
	CRC_FIRST_BIT = 52,
	CRC_BITS = 16,
	FILL_FIRST_BIT = 68,
	FILL_BITS = 4,
};

static uint16_t payload_crc(uint32_t payload)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = 0; index < PCM_J_FRAME_PAYLOAD_BITS; ++index)
		remainder = crc16_ccitt_false_step(
			remainder,
			(payload & (UINT32_C(1) << index)) != 0);
	return remainder;
}

static void encode_payload(uint8_t *frame, uint32_t payload)
{
	packed_msb_write_lsb_field(
		frame, FIRST_PAYLOAD_BIT, FIRST_PAYLOAD_BITS, payload);
	packed_msb_write_lsb_field(
		frame, SECOND_PAYLOAD_BIT, SECOND_PAYLOAD_BITS,
		payload >> FIRST_PAYLOAD_BITS);
}

static void encode_crc(uint8_t *frame, uint16_t crc)
{
	packed_msb_write_msb_field(
		frame, CRC_FIRST_BIT, CRC_BITS, crc);
}

enum pcm_j_frame_result pcm_j_frame_encode(
	uint32_t payload, uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[PCM_J_FRAME_BYTES] = { 0 };

	if (frame == NULL || frame_capacity < sizeof(encoded))
		return PCM_J_FRAME_INVALID_ARGUMENT;
	for (size_t index = FRAME_SYNC_FIRST_BIT;
	     index < PCM_J_FRAME_SYNC_BITS; ++index)
		packed_msb_set_bit(encoded, index, true);
	encode_payload(encoded, payload);
	encode_crc(encoded, payload_crc(payload));
	memcpy(frame, encoded, sizeof(encoded));
	return PCM_J_FRAME_OK;
}

static bool framing_is_valid(const uint8_t *frame)
{
	for (size_t index = FRAME_SYNC_FIRST_BIT;
	     index < PCM_J_FRAME_SYNC_BITS; ++index) {
		if (packed_msb_bit(frame, index) == 0)
			return false;
	}
	if (packed_msb_bit(frame, START0_BIT) != 0 ||
	    packed_msb_bit(frame, START1_BIT) != 0 ||
	    packed_msb_bit(frame, START2_BIT) != 0)
		return false;
	return packed_msb_bits_are_zero(frame, FILL_FIRST_BIT, FILL_BITS);
}

static uint32_t decode_payload(const uint8_t *frame)
{
	return packed_msb_read_lsb_field(
		       frame, FIRST_PAYLOAD_BIT, FIRST_PAYLOAD_BITS) |
	       packed_msb_read_lsb_field(
		       frame, SECOND_PAYLOAD_BIT, SECOND_PAYLOAD_BITS)
		       << FIRST_PAYLOAD_BITS;
}

static uint16_t decode_crc(const uint8_t *frame)
{
	return (uint16_t)packed_msb_read_msb_field(
		frame, CRC_FIRST_BIT, CRC_BITS);
}

enum pcm_j_frame_result pcm_j_frame_decode(
	const uint8_t *frame, size_t frame_capacity, uint32_t *payload)
{
	uint32_t decoded;

	if (frame == NULL || payload == NULL ||
	    frame_capacity < PCM_J_FRAME_BYTES)
		return PCM_J_FRAME_INVALID_ARGUMENT;
	if (!framing_is_valid(frame))
		return PCM_J_FRAME_INVALID_FRAME;
	decoded = decode_payload(frame);
	if (decode_crc(frame) != payload_crc(decoded))
		return PCM_J_FRAME_CRC_MISMATCH;
	*payload = decoded;
	return PCM_J_FRAME_OK;
}

const char *pcm_j_frame_result_name(enum pcm_j_frame_result result)
{
	switch (result) {
	case PCM_J_FRAME_OK:
		return "ok";
	case PCM_J_FRAME_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_J_FRAME_INVALID_FRAME:
		return "invalid frame";
	case PCM_J_FRAME_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
