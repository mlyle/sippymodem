// SPDX-License-Identifier: BSD-3-Clause

#include "v90_jd.h"

#include "crc16_ccitt_false.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	FRAME_SYNC_FIRST_BIT = 0,
	START0_BIT = 17,
	FIRST_RATE_FIELD_BIT = 18,
	FIRST_RATE_FIELD_BITS = 16,
	START1_BIT = 34,
	SECOND_RATE_FIELD_BIT = 35,
	SECOND_RATE_FIELD_BITS = 6,
	TRAINING_CONSTELLATION_BIT = 47,
	RENEGOTIATION_CONSTELLATION_BIT = 48,
	LOOKAHEAD_FIRST_BIT = 49,
	LOOKAHEAD_BITS = 2,
	START2_BIT = 51,
	CRC_FIRST_BIT = 52,
	CRC_BITS = 16,
	FILL_FIRST_BIT = 68,
	FILL_BITS = 4,
	FIELD_STRIDE_BITS = 17,
};

static bool constellation_is_valid(
	enum v34_constellation_size constellation)
{
	return constellation == V34_CONSTELLATION_4_POINT ||
	       constellation == V34_CONSTELLATION_16_POINT;
}

bool v90_jd_parameters_are_valid(
	const struct v90_jd_parameters *parameters)
{
	return parameters != NULL &&
	       (parameters->data_rate_capability_mask &
		~(uint32_t)V90_JD_RATE_CAPABILITY_MASK) == 0 &&
	       constellation_is_valid(parameters->training_constellation) &&
	       constellation_is_valid(
		       parameters->rate_renegotiation_constellation) &&
	       parameters->maximum_spectral_shaping_lookahead >=
		       V90_JD_MINIMUM_LOOKAHEAD &&
	       parameters->maximum_spectral_shaping_lookahead <=
		       V90_JD_MAXIMUM_LOOKAHEAD;
}

static uint8_t frame_bit(const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void set_frame_bit(uint8_t *frame, size_t bit_index, bool value)
{
	if (value)
		frame[bit_index / 8u] |=
			(uint8_t)(1u << (7u - bit_index % 8u));
}

static uint32_t decode_lsb_first_field(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	uint32_t value = 0;

	for (size_t index = 0; index < bit_count; ++index)
		value |= (uint32_t)frame_bit(frame, first_bit + index) << index;
	return value;
}

static void encode_lsb_first_field(
	uint8_t *frame, size_t first_bit, size_t bit_count, uint32_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		set_frame_bit(
			frame, first_bit + index,
			(value & (UINT32_C(1) << index)) != 0);
}

static uint16_t decode_msb_first_crc(const uint8_t *frame)
{
	uint16_t value = 0;

	for (size_t index = 0; index < CRC_BITS; ++index)
		value = (uint16_t)((value << 1) |
				   frame_bit(frame, CRC_FIRST_BIT + index));
	return value;
}

static void encode_msb_first_crc(uint8_t *frame, uint16_t value)
{
	for (size_t index = 0; index < CRC_BITS; ++index)
		set_frame_bit(
			frame, CRC_FIRST_BIT + index,
			(value & (uint16_t)(1u << (CRC_BITS - index - 1u))) != 0);
}

static bool is_start_bit(size_t bit_index)
{
	return bit_index >= START0_BIT && bit_index <= START2_BIT &&
	       (bit_index - START0_BIT) % FIELD_STRIDE_BITS == 0;
}

static uint16_t frame_crc(const uint8_t *frame)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = FIRST_RATE_FIELD_BIT;
	     index < CRC_FIRST_BIT; ++index) {
		if (!is_start_bit(index))
			remainder = crc16_ccitt_false_step(
				remainder, frame_bit(frame, index) != 0);
	}
	return remainder;
}

enum v90_jd_result v90_jd_encode(
	const struct v90_jd_parameters *parameters,
	uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[V90_JD_FRAME_BYTES] = { 0 };

	if (parameters == NULL || frame == NULL ||
	    frame_capacity < sizeof(encoded))
		return V90_JD_INVALID_ARGUMENT;
	if (!v90_jd_parameters_are_valid(parameters))
		return V90_JD_INVALID_PARAMETERS;
	for (size_t index = FRAME_SYNC_FIRST_BIT;
	     index < V90_JD_FRAME_SYNC_BITS; ++index)
		set_frame_bit(encoded, index, true);
	encode_lsb_first_field(
		encoded, FIRST_RATE_FIELD_BIT, FIRST_RATE_FIELD_BITS,
		parameters->data_rate_capability_mask);
	encode_lsb_first_field(
		encoded, SECOND_RATE_FIELD_BIT, SECOND_RATE_FIELD_BITS,
		parameters->data_rate_capability_mask >> FIRST_RATE_FIELD_BITS);
	set_frame_bit(
		encoded, TRAINING_CONSTELLATION_BIT,
		parameters->training_constellation == V34_CONSTELLATION_16_POINT);
	set_frame_bit(
		encoded, RENEGOTIATION_CONSTELLATION_BIT,
		parameters->rate_renegotiation_constellation ==
			V34_CONSTELLATION_16_POINT);
	encode_lsb_first_field(
		encoded, LOOKAHEAD_FIRST_BIT, LOOKAHEAD_BITS,
		parameters->maximum_spectral_shaping_lookahead);
	encode_msb_first_crc(encoded, frame_crc(encoded));
	memcpy(frame, encoded, sizeof(encoded));
	return V90_JD_OK;
}

static bool framing_is_valid(const uint8_t *frame)
{
	for (size_t index = FRAME_SYNC_FIRST_BIT;
	     index < V90_JD_FRAME_SYNC_BITS; ++index) {
		if (frame_bit(frame, index) == 0)
			return false;
	}
	if (frame_bit(frame, START0_BIT) != 0 ||
	    frame_bit(frame, START1_BIT) != 0 ||
	    frame_bit(frame, START2_BIT) != 0)
		return false;
	for (size_t index = FILL_FIRST_BIT;
	     index < FILL_FIRST_BIT + FILL_BITS; ++index) {
		if (frame_bit(frame, index) != 0)
			return false;
	}
	return true;
}

enum v90_jd_result v90_jd_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v90_jd_parameters *parameters)
{
	struct v90_jd_parameters decoded;

	if (frame == NULL || parameters == NULL ||
	    frame_capacity < V90_JD_FRAME_BYTES)
		return V90_JD_INVALID_ARGUMENT;
	if (!framing_is_valid(frame))
		return V90_JD_INVALID_FRAME;
	if (decode_msb_first_crc(frame) != frame_crc(frame))
		return V90_JD_CRC_MISMATCH;
	decoded = (struct v90_jd_parameters){
		.data_rate_capability_mask =
			decode_lsb_first_field(
				frame, FIRST_RATE_FIELD_BIT,
				FIRST_RATE_FIELD_BITS) |
			(decode_lsb_first_field(
				 frame, SECOND_RATE_FIELD_BIT,
				 SECOND_RATE_FIELD_BITS)
			 << FIRST_RATE_FIELD_BITS),
		.training_constellation =
			frame_bit(frame, TRAINING_CONSTELLATION_BIT) != 0 ?
				V34_CONSTELLATION_16_POINT :
				V34_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation =
			frame_bit(frame, RENEGOTIATION_CONSTELLATION_BIT) != 0 ?
				V34_CONSTELLATION_16_POINT :
				V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead =
			(uint8_t)decode_lsb_first_field(
				frame, LOOKAHEAD_FIRST_BIT, LOOKAHEAD_BITS),
	};
	if (!v90_jd_parameters_are_valid(&decoded))
		return V90_JD_INVALID_FRAME;
	*parameters = decoded;
	return V90_JD_OK;
}

const char *v90_jd_result_name(enum v90_jd_result result)
{
	switch (result) {
	case V90_JD_OK:
		return "ok";
	case V90_JD_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_JD_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_JD_INVALID_FRAME:
		return "invalid frame";
	case V90_JD_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
