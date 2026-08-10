// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_mp.h"

#include "sippymodem_crc16_ccitt_false.h"

#include <string.h>

enum {
	V34_MP_FRAME_SYNC_FIRST_BIT = 0,
	V34_MP_START0_BIT = 17,
	V34_MP_TYPE_BIT = 18,
	V34_MP_MAXIMUM_CALL_TO_ANSWER_RATE_FIRST_BIT = 20,
	V34_MP_MAXIMUM_ANSWER_TO_CALL_RATE_FIRST_BIT = 24,
	V34_MP_RATE_FIELD_BITS = 4,
	V34_MP_AUXILIARY_CHANNEL_BIT = 28,
	V34_MP_TRELLIS_FIRST_BIT = 29,
	V34_MP_TRELLIS_BITS = 2,
	V34_MP_NONLINEAR_ENCODING_BIT = 31,
	V34_MP_EXPANDED_SHAPING_BIT = 32,
	V34_MP_ACKNOWLEDGMENT_BIT = 33,
	V34_MP_START1_BIT = 34,
	V34_MP_RATE_CAPABILITY_FIRST_BIT = 35,
	V34_MP_ASYMMETRIC_RATES_BIT = 50,
	V34_MP_START2_BIT = 51,
	V34_MP_START3_BIT = 68,
	V34_MP_TYPE0_CRC_FIRST_BIT = 69,
	V34_MP_TYPE0_FILL_FIRST_BIT = 85,
	V34_MP_TYPE0_FILL_BITS = 3,
};

static bool trellis_is_valid(
	enum sippymodem_v34_trellis_selection trellis)
{
	return trellis == SIPPYMODEM_V34_TRELLIS_16_STATE ||
	       trellis == SIPPYMODEM_V34_TRELLIS_32_STATE ||
	       trellis == SIPPYMODEM_V34_TRELLIS_64_STATE;
}

static bool rate_is_valid(uint8_t rate_2400_bps_units)
{
	return rate_2400_bps_units >=
		       SIPPYMODEM_V34_MP_MINIMUM_RATE_2400_BPS_UNITS &&
	       rate_2400_bps_units <=
		       SIPPYMODEM_V34_MP_MAXIMUM_RATE_2400_BPS_UNITS;
}

bool sippymodem_v34_mp_parameters_are_valid(
	const struct sippymodem_v34_mp_parameters *parameters)
{
	return parameters != NULL &&
	       rate_is_valid(
		       parameters->maximum_call_to_answer_rate_2400_bps_units) &&
	       rate_is_valid(
		       parameters->maximum_answer_to_call_rate_2400_bps_units) &&
	       trellis_is_valid(parameters->remote_transmitter_trellis) &&
	       (parameters->data_rate_capability_mask &
		~SIPPYMODEM_V34_MP_RATE_CAPABILITY_MASK) == 0;
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

static uint16_t decode_lsb_first_field(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	uint16_t value = 0;

	for (size_t index = 0; index < bit_count; ++index)
		value |= (uint16_t)frame_bit(frame, first_bit + index) << index;
	return value;
}

static void encode_lsb_first_field(
	uint8_t *frame, size_t first_bit, size_t bit_count, uint16_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		set_frame_bit(
			frame, first_bit + index,
			(value & (uint16_t)(1u << index)) != 0);
}

static uint16_t decode_msb_first_field(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	uint16_t value = 0;

	for (size_t index = 0; index < bit_count; ++index)
		value = (uint16_t)((value << 1) |
				   frame_bit(frame, first_bit + index));
	return value;
}

static void encode_msb_first_field(
	uint8_t *frame, size_t first_bit, size_t bit_count, uint16_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		set_frame_bit(
			frame, first_bit + index,
			(value & (uint16_t)(1u << (bit_count - index - 1u))) !=
				0);
}

static bool is_start_bit(size_t bit_index)
{
	return bit_index == V34_MP_START0_BIT ||
	       bit_index == V34_MP_START1_BIT ||
	       bit_index == V34_MP_START2_BIT ||
	       bit_index == V34_MP_START3_BIT;
}

static uint16_t type0_crc(const uint8_t *frame)
{
	uint16_t remainder =
		SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = V34_MP_TYPE_BIT;
	     index < V34_MP_TYPE0_CRC_FIRST_BIT; ++index) {
		if (!is_start_bit(index))
			remainder = sippymodem_crc16_ccitt_false_step(
				remainder, frame_bit(frame, index) != 0);
	}
	return remainder;
}

enum sippymodem_v34_mp_result sippymodem_v34_mp_type0_encode(
	const struct sippymodem_v34_mp_parameters *parameters,
	uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES] = { 0 };
	uint16_t crc;

	if (parameters == NULL || frame == NULL ||
	    frame_capacity < SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES)
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	if (!sippymodem_v34_mp_parameters_are_valid(parameters))
		return SIPPYMODEM_V34_MP_INVALID_PARAMETERS;

	for (size_t index = V34_MP_FRAME_SYNC_FIRST_BIT;
	     index < SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS; ++index)
		set_frame_bit(encoded, index, true);
	encode_lsb_first_field(
		encoded, V34_MP_MAXIMUM_CALL_TO_ANSWER_RATE_FIRST_BIT,
		V34_MP_RATE_FIELD_BITS,
		parameters->maximum_call_to_answer_rate_2400_bps_units);
	encode_lsb_first_field(
		encoded, V34_MP_MAXIMUM_ANSWER_TO_CALL_RATE_FIRST_BIT,
		V34_MP_RATE_FIELD_BITS,
		parameters->maximum_answer_to_call_rate_2400_bps_units);
	set_frame_bit(
		encoded, V34_MP_AUXILIARY_CHANNEL_BIT,
		parameters->auxiliary_channel_enabled);
	encode_lsb_first_field(
		encoded, V34_MP_TRELLIS_FIRST_BIT, V34_MP_TRELLIS_BITS,
		(uint16_t)parameters->remote_transmitter_trellis);
	set_frame_bit(
		encoded, V34_MP_NONLINEAR_ENCODING_BIT,
		parameters->remote_transmitter_nonlinear_encoding);
	set_frame_bit(
		encoded, V34_MP_EXPANDED_SHAPING_BIT,
		parameters->remote_transmitter_expanded_shaping);
	set_frame_bit(
		encoded, V34_MP_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_peer_mp);
	encode_lsb_first_field(
		encoded, V34_MP_RATE_CAPABILITY_FIRST_BIT,
		SIPPYMODEM_V34_MP_RATE_CAPABILITY_BITS,
		parameters->data_rate_capability_mask);
	set_frame_bit(
		encoded, V34_MP_ASYMMETRIC_RATES_BIT,
		parameters->asymmetric_rates_enabled);

	crc = type0_crc(encoded);
	/* The CCITT-FALSE numeric representation holds the first V.34 CRC
	 * output in its most-significant bit. */
	encode_msb_first_field(
		encoded, V34_MP_TYPE0_CRC_FIRST_BIT,
		SIPPYMODEM_V34_MP_CRC_BITS, crc);
	memcpy(frame, encoded, sizeof(encoded));
	return SIPPYMODEM_V34_MP_OK;
}

static bool frame_sync_is_valid(const uint8_t *frame)
{
	for (size_t index = V34_MP_FRAME_SYNC_FIRST_BIT;
	     index < SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS; ++index) {
		if (frame_bit(frame, index) == 0)
			return false;
	}
	return true;
}

static bool framing_bits_are_valid(const uint8_t *frame)
{
	if (!frame_sync_is_valid(frame) ||
	    frame_bit(frame, V34_MP_START0_BIT) != 0 ||
	    frame_bit(frame, V34_MP_START1_BIT) != 0 ||
	    frame_bit(frame, V34_MP_START2_BIT) != 0 ||
	    frame_bit(frame, V34_MP_START3_BIT) != 0 ||
	    frame_bit(frame, V34_MP_TYPE_BIT) != 0)
		return false;
	for (size_t index = V34_MP_TYPE0_FILL_FIRST_BIT;
	     index < V34_MP_TYPE0_FILL_FIRST_BIT + V34_MP_TYPE0_FILL_BITS;
	     ++index) {
		if (frame_bit(frame, index) != 0)
			return false;
	}
	return true;
}

enum sippymodem_v34_mp_result sippymodem_v34_mp_type0_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct sippymodem_v34_mp_parameters *parameters)
{
	struct sippymodem_v34_mp_parameters decoded = { 0 };
	uint16_t received_crc;

	if (frame == NULL || parameters == NULL ||
	    frame_capacity < SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES)
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	if (!framing_bits_are_valid(frame))
		return SIPPYMODEM_V34_MP_INVALID_FRAME;
	received_crc = decode_msb_first_field(
		frame, V34_MP_TYPE0_CRC_FIRST_BIT,
		SIPPYMODEM_V34_MP_CRC_BITS);
	if (received_crc != type0_crc(frame))
		return SIPPYMODEM_V34_MP_CRC_MISMATCH;

	decoded.maximum_call_to_answer_rate_2400_bps_units =
		(uint8_t)decode_lsb_first_field(
			frame,
			V34_MP_MAXIMUM_CALL_TO_ANSWER_RATE_FIRST_BIT,
			V34_MP_RATE_FIELD_BITS);
	decoded.maximum_answer_to_call_rate_2400_bps_units =
		(uint8_t)decode_lsb_first_field(
			frame,
			V34_MP_MAXIMUM_ANSWER_TO_CALL_RATE_FIRST_BIT,
			V34_MP_RATE_FIELD_BITS);
	decoded.auxiliary_channel_enabled =
		frame_bit(frame, V34_MP_AUXILIARY_CHANNEL_BIT) != 0;
	decoded.remote_transmitter_trellis =
		(enum sippymodem_v34_trellis_selection)decode_lsb_first_field(
			frame, V34_MP_TRELLIS_FIRST_BIT, V34_MP_TRELLIS_BITS);
	decoded.remote_transmitter_nonlinear_encoding =
		frame_bit(frame, V34_MP_NONLINEAR_ENCODING_BIT) != 0;
	decoded.remote_transmitter_expanded_shaping =
		frame_bit(frame, V34_MP_EXPANDED_SHAPING_BIT) != 0;
	decoded.acknowledges_peer_mp =
		frame_bit(frame, V34_MP_ACKNOWLEDGMENT_BIT) != 0;
	decoded.data_rate_capability_mask = decode_lsb_first_field(
		frame, V34_MP_RATE_CAPABILITY_FIRST_BIT,
		SIPPYMODEM_V34_MP_RATE_CAPABILITY_BITS);
	decoded.asymmetric_rates_enabled =
		frame_bit(frame, V34_MP_ASYMMETRIC_RATES_BIT) != 0;
	if (!sippymodem_v34_mp_parameters_are_valid(&decoded))
		return SIPPYMODEM_V34_MP_INVALID_FRAME;
	*parameters = decoded;
	return SIPPYMODEM_V34_MP_OK;
}

const char *sippymodem_v34_mp_result_name(
	enum sippymodem_v34_mp_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MP_OK:
		return "ok";
	case SIPPYMODEM_V34_MP_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_MP_INVALID_PARAMETERS:
		return "invalid parameters";
	case SIPPYMODEM_V34_MP_INVALID_FRAME:
		return "invalid frame";
	case SIPPYMODEM_V34_MP_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
