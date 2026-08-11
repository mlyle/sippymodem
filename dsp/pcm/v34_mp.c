// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mp.h"

#include "crc16_ccitt_false.h"

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
	V34_MP_RATE_CAPABILITY_FIRST_BIT = 35,
	V34_MP_ASYMMETRIC_RATES_BIT = 50,
	V34_MP_TYPE1_FIRST_COEFFICIENT_BIT = 52,
	V34_MP_COEFFICIENT_COMPONENT_BITS = 16,
	V34_MP_FIELD_STRIDE_BITS = 17,
	V34_MP_TYPE0_CRC_FIRST_BIT = 69,
	V34_MP_TYPE0_FILL_FIRST_BIT = 85,
	V34_MP_TYPE0_FILL_BITS = 3,
	V34_MP_TYPE1_CRC_FIRST_BIT = 171,
	V34_MP_TYPE1_FILL_FIRST_BIT = 187,
	V34_MP_TYPE1_FILL_BITS = 1,
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

bool sippymodem_v34_mp_type1_parameters_are_valid(
	const struct sippymodem_v34_mp_type1_parameters *parameters)
{
	if (parameters == NULL ||
	    !sippymodem_v34_mp_parameters_are_valid(&parameters->common))
		return false;
	for (size_t index = 0; index < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++index) {
		if (parameters->precoder_coefficients[index].real == INT16_MIN ||
		    parameters->precoder_coefficients[index].imaginary ==
			    INT16_MIN)
			return false;
	}
	return true;
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
	return bit_index >= V34_MP_START0_BIT &&
	       bit_index < V34_MP_TYPE1_CRC_FIRST_BIT &&
	       (bit_index - V34_MP_START0_BIT) % V34_MP_FIELD_STRIDE_BITS == 0;
}

static uint16_t frame_crc(const uint8_t *frame, size_t crc_first_bit)
{
	uint16_t remainder =
		SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = V34_MP_TYPE_BIT; index < crc_first_bit; ++index) {
		if (!is_start_bit(index))
			remainder = sippymodem_crc16_ccitt_false_step(
				remainder, frame_bit(frame, index) != 0);
	}
	return remainder;
}

static void encode_common_parameters(
	uint8_t *frame, const struct sippymodem_v34_mp_parameters *parameters,
	bool type1)
{
	for (size_t index = V34_MP_FRAME_SYNC_FIRST_BIT;
	     index < SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS; ++index)
		set_frame_bit(frame, index, true);
	set_frame_bit(frame, V34_MP_TYPE_BIT, type1);
	encode_lsb_first_field(
		frame, V34_MP_MAXIMUM_CALL_TO_ANSWER_RATE_FIRST_BIT,
		V34_MP_RATE_FIELD_BITS,
		parameters->maximum_call_to_answer_rate_2400_bps_units);
	encode_lsb_first_field(
		frame, V34_MP_MAXIMUM_ANSWER_TO_CALL_RATE_FIRST_BIT,
		V34_MP_RATE_FIELD_BITS,
		parameters->maximum_answer_to_call_rate_2400_bps_units);
	set_frame_bit(
		frame, V34_MP_AUXILIARY_CHANNEL_BIT,
		parameters->auxiliary_channel_enabled);
	encode_lsb_first_field(
		frame, V34_MP_TRELLIS_FIRST_BIT, V34_MP_TRELLIS_BITS,
		(uint16_t)parameters->remote_transmitter_trellis);
	set_frame_bit(
		frame, V34_MP_NONLINEAR_ENCODING_BIT,
		parameters->remote_transmitter_nonlinear_encoding);
	set_frame_bit(
		frame, V34_MP_EXPANDED_SHAPING_BIT,
		parameters->remote_transmitter_expanded_shaping);
	set_frame_bit(
		frame, V34_MP_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_peer_mp);
	encode_lsb_first_field(
		frame, V34_MP_RATE_CAPABILITY_FIRST_BIT,
		SIPPYMODEM_V34_MP_RATE_CAPABILITY_BITS,
		parameters->data_rate_capability_mask);
	set_frame_bit(
		frame, V34_MP_ASYMMETRIC_RATES_BIT,
		parameters->asymmetric_rates_enabled);
}

static void decode_common_parameters(
	const uint8_t *frame, struct sippymodem_v34_mp_parameters *parameters)
{
	parameters->maximum_call_to_answer_rate_2400_bps_units =
		(uint8_t)decode_lsb_first_field(
			frame,
			V34_MP_MAXIMUM_CALL_TO_ANSWER_RATE_FIRST_BIT,
			V34_MP_RATE_FIELD_BITS);
	parameters->maximum_answer_to_call_rate_2400_bps_units =
		(uint8_t)decode_lsb_first_field(
			frame,
			V34_MP_MAXIMUM_ANSWER_TO_CALL_RATE_FIRST_BIT,
			V34_MP_RATE_FIELD_BITS);
	parameters->auxiliary_channel_enabled =
		frame_bit(frame, V34_MP_AUXILIARY_CHANNEL_BIT) != 0;
	parameters->remote_transmitter_trellis =
		(enum sippymodem_v34_trellis_selection)decode_lsb_first_field(
			frame, V34_MP_TRELLIS_FIRST_BIT, V34_MP_TRELLIS_BITS);
	parameters->remote_transmitter_nonlinear_encoding =
		frame_bit(frame, V34_MP_NONLINEAR_ENCODING_BIT) != 0;
	parameters->remote_transmitter_expanded_shaping =
		frame_bit(frame, V34_MP_EXPANDED_SHAPING_BIT) != 0;
	parameters->acknowledges_peer_mp =
		frame_bit(frame, V34_MP_ACKNOWLEDGMENT_BIT) != 0;
	parameters->data_rate_capability_mask = decode_lsb_first_field(
		frame, V34_MP_RATE_CAPABILITY_FIRST_BIT,
		SIPPYMODEM_V34_MP_RATE_CAPABILITY_BITS);
	parameters->asymmetric_rates_enabled =
		frame_bit(frame, V34_MP_ASYMMETRIC_RATES_BIT) != 0;
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

	encode_common_parameters(encoded, parameters, false);

	crc = frame_crc(encoded, V34_MP_TYPE0_CRC_FIRST_BIT);
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

static bool framing_bits_are_valid(
	const uint8_t *frame, bool type1, size_t crc_first_bit,
	size_t fill_first_bit, size_t fill_bit_count)
{
	if (!frame_sync_is_valid(frame) ||
	    frame_bit(frame, V34_MP_TYPE_BIT) != (uint8_t)type1)
		return false;
	for (size_t index = V34_MP_START0_BIT; index < crc_first_bit;
	     index += V34_MP_FIELD_STRIDE_BITS) {
		if (frame_bit(frame, index) != 0)
			return false;
	}
	for (size_t index = fill_first_bit;
	     index < fill_first_bit + fill_bit_count; ++index) {
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
	if (!framing_bits_are_valid(
		    frame, false, V34_MP_TYPE0_CRC_FIRST_BIT,
		    V34_MP_TYPE0_FILL_FIRST_BIT, V34_MP_TYPE0_FILL_BITS))
		return SIPPYMODEM_V34_MP_INVALID_FRAME;
	received_crc = decode_msb_first_field(
		frame, V34_MP_TYPE0_CRC_FIRST_BIT,
		SIPPYMODEM_V34_MP_CRC_BITS);
	if (received_crc != frame_crc(frame, V34_MP_TYPE0_CRC_FIRST_BIT))
		return SIPPYMODEM_V34_MP_CRC_MISMATCH;

	decode_common_parameters(frame, &decoded);
	if (!sippymodem_v34_mp_parameters_are_valid(&decoded))
		return SIPPYMODEM_V34_MP_INVALID_FRAME;
	*parameters = decoded;
	return SIPPYMODEM_V34_MP_OK;
}

static size_t coefficient_component_first_bit(
	size_t tap_index, bool imaginary)
{
	return V34_MP_TYPE1_FIRST_COEFFICIENT_BIT +
	       (tap_index * 2u + (imaginary ? 1u : 0u)) *
		       V34_MP_FIELD_STRIDE_BITS;
}

static int16_t decode_twos_complement(uint16_t value)
{
	if (value <= INT16_MAX)
		return (int16_t)value;
	return (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

enum sippymodem_v34_mp_result sippymodem_v34_mp_type1_encode(
	const struct sippymodem_v34_mp_type1_parameters *parameters,
	uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES] = { 0 };
	uint16_t crc;

	if (parameters == NULL || frame == NULL ||
	    frame_capacity < SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES)
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	if (!sippymodem_v34_mp_type1_parameters_are_valid(parameters))
		return SIPPYMODEM_V34_MP_INVALID_PARAMETERS;

	encode_common_parameters(encoded, &parameters->common, true);
	for (size_t index = 0; index < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++index) {
		encode_lsb_first_field(
			encoded, coefficient_component_first_bit(index, false),
			V34_MP_COEFFICIENT_COMPONENT_BITS,
			(uint16_t)parameters->precoder_coefficients[index].real);
		encode_lsb_first_field(
			encoded, coefficient_component_first_bit(index, true),
			V34_MP_COEFFICIENT_COMPONENT_BITS,
			(uint16_t)parameters->precoder_coefficients[index].imaginary);
	}
	crc = frame_crc(encoded, V34_MP_TYPE1_CRC_FIRST_BIT);
	encode_msb_first_field(
		encoded, V34_MP_TYPE1_CRC_FIRST_BIT,
		SIPPYMODEM_V34_MP_CRC_BITS, crc);
	memcpy(frame, encoded, sizeof(encoded));
	return SIPPYMODEM_V34_MP_OK;
}

enum sippymodem_v34_mp_result sippymodem_v34_mp_type1_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct sippymodem_v34_mp_type1_parameters *parameters)
{
	struct sippymodem_v34_mp_type1_parameters decoded = { 0 };
	uint16_t received_crc;

	if (frame == NULL || parameters == NULL ||
	    frame_capacity < SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES)
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	if (!framing_bits_are_valid(
		    frame, true, V34_MP_TYPE1_CRC_FIRST_BIT,
		    V34_MP_TYPE1_FILL_FIRST_BIT, V34_MP_TYPE1_FILL_BITS))
		return SIPPYMODEM_V34_MP_INVALID_FRAME;
	received_crc = decode_msb_first_field(
		frame, V34_MP_TYPE1_CRC_FIRST_BIT,
		SIPPYMODEM_V34_MP_CRC_BITS);
	if (received_crc != frame_crc(frame, V34_MP_TYPE1_CRC_FIRST_BIT))
		return SIPPYMODEM_V34_MP_CRC_MISMATCH;

	decode_common_parameters(frame, &decoded.common);
	for (size_t index = 0; index < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++index) {
		decoded.precoder_coefficients[index].real =
			decode_twos_complement(decode_lsb_first_field(
				frame,
				coefficient_component_first_bit(index, false),
				V34_MP_COEFFICIENT_COMPONENT_BITS));
		decoded.precoder_coefficients[index].imaginary =
			decode_twos_complement(decode_lsb_first_field(
				frame,
				coefficient_component_first_bit(index, true),
				V34_MP_COEFFICIENT_COMPONENT_BITS));
	}
	if (!sippymodem_v34_mp_type1_parameters_are_valid(&decoded))
		return SIPPYMODEM_V34_MP_INVALID_FRAME;
	*parameters = decoded;
	return SIPPYMODEM_V34_MP_OK;
}

static bool packed_tail_is_zero(
	const uint8_t *frame, size_t used_bit_count)
{
	size_t byte_index = used_bit_count / 8u;
	size_t used_bits = used_bit_count % 8u;

	if (used_bits != 0) {
		uint8_t unused_mask =
			(uint8_t)((1u << (8u - used_bits)) - 1u);

		if ((frame[byte_index] & unused_mask) != 0)
			return false;
		++byte_index;
	}
	for (; byte_index < SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES;
	     ++byte_index) {
		if (frame[byte_index] != 0)
			return false;
	}
	return true;
}

static bool receive_prefix_is_valid(const uint8_t *frame)
{
	return frame_sync_is_valid(frame) &&
	       frame_bit(frame, V34_MP_START0_BIT) == 0;
}

static bool receive_stage_is_valid(enum sippymodem_v34_mp_receive_stage stage)
{
	return stage == SIPPYMODEM_V34_MP_SEARCH_SYNC ||
	       stage == SIPPYMODEM_V34_MP_RECEIVE_TYPE ||
	       stage == SIPPYMODEM_V34_MP_RECEIVE_FRAME;
}

bool sippymodem_v34_mp_receiver_is_valid(
	const struct sippymodem_v34_mp_receiver *receiver)
{
	if (receiver == NULL ||
	    receiver->sync_one_count > SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS ||
	    receiver->bit_index > SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS ||
	    !receive_stage_is_valid(receiver->stage) ||
	    !packed_tail_is_zero(receiver->frame, receiver->bit_index))
		return false;
	switch (receiver->stage) {
	case SIPPYMODEM_V34_MP_SEARCH_SYNC:
		return receiver->frame_bit_count == 0 &&
		       receiver->bit_index == 0;
	case SIPPYMODEM_V34_MP_RECEIVE_TYPE:
		return receiver->frame_bit_count == 0 &&
		       receiver->bit_index == V34_MP_TYPE_BIT &&
		       receiver->sync_one_count == 0 &&
		       receive_prefix_is_valid(receiver->frame);
	case SIPPYMODEM_V34_MP_RECEIVE_FRAME:
		return (receiver->frame_bit_count ==
				SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS ||
			receiver->frame_bit_count ==
				SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS) &&
		       receiver->bit_index > V34_MP_TYPE_BIT &&
		       receiver->bit_index < receiver->frame_bit_count &&
		       receiver->sync_one_count == 0 &&
		       receive_prefix_is_valid(receiver->frame) &&
		       frame_bit(receiver->frame, V34_MP_TYPE_BIT) ==
			       (uint8_t)(receiver->frame_bit_count ==
					 SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS);
	}
	return false;
}

enum sippymodem_v34_mp_result sippymodem_v34_mp_receiver_init(
	struct sippymodem_v34_mp_receiver *receiver)
{
	struct sippymodem_v34_mp_receiver initialized = {
		.stage = SIPPYMODEM_V34_MP_SEARCH_SYNC,
	};

	if (receiver == NULL ||
	    !sippymodem_v34_mp_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_MP_OK;
}

static void clear_receive_frame(struct sippymodem_v34_mp_receiver *receiver)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->frame_bit_count = 0;
	receiver->bit_index = 0;
	receiver->sync_one_count = 0;
	receiver->stage = SIPPYMODEM_V34_MP_SEARCH_SYNC;
}

static void begin_receive_frame(struct sippymodem_v34_mp_receiver *receiver)
{
	clear_receive_frame(receiver);
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS; ++index)
		set_frame_bit(receiver->frame, index, true);
	receiver->bit_index = V34_MP_TYPE_BIT;
	receiver->stage = SIPPYMODEM_V34_MP_RECEIVE_TYPE;
}

static void complete_receive_frame(
	struct sippymodem_v34_mp_receiver *receiver,
	struct sippymodem_v34_mp_receive_event *event)
{
	enum sippymodem_v34_mp_result result;

	if (receiver->frame_bit_count == SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS) {
		struct sippymodem_v34_mp_parameters parameters;

		result = sippymodem_v34_mp_type0_decode(
			receiver->frame, sizeof(receiver->frame), &parameters);
		if (result == SIPPYMODEM_V34_MP_OK) {
			event->type = SIPPYMODEM_V34_MP_RECEIVE_TYPE0;
			event->parameters.type0 = parameters;
		}
	} else {
		struct sippymodem_v34_mp_type1_parameters parameters;

		result = sippymodem_v34_mp_type1_decode(
			receiver->frame, sizeof(receiver->frame), &parameters);
		if (result == SIPPYMODEM_V34_MP_OK) {
			event->type = SIPPYMODEM_V34_MP_RECEIVE_TYPE1;
			event->parameters.type1 = parameters;
		}
	}
	event->frame_result = result;
	if (result != SIPPYMODEM_V34_MP_OK)
		event->type = SIPPYMODEM_V34_MP_RECEIVE_REJECTED;
	clear_receive_frame(receiver);
}

static void receive_frame_bit(
	struct sippymodem_v34_mp_receiver *receiver, uint8_t bit,
	struct sippymodem_v34_mp_receive_event *event)
{
	if (receiver->stage == SIPPYMODEM_V34_MP_SEARCH_SYNC) {
		if (bit != 0) {
			if (receiver->sync_one_count <
			    SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS)
				receiver->sync_one_count++;
		} else if (receiver->sync_one_count ==
			   SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS) {
			begin_receive_frame(receiver);
		} else {
			receiver->sync_one_count = 0;
		}
		return;
	}

	set_frame_bit(receiver->frame, receiver->bit_index, bit != 0);
	receiver->bit_index++;
	if (receiver->stage == SIPPYMODEM_V34_MP_RECEIVE_TYPE) {
		receiver->frame_bit_count = bit != 0 ?
			SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS :
			SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS;
		receiver->stage = SIPPYMODEM_V34_MP_RECEIVE_FRAME;
	}
	if (receiver->bit_index == receiver->frame_bit_count)
		complete_receive_frame(receiver, event);
}

enum sippymodem_v34_mp_result sippymodem_v34_mp_receive_bit(
	struct sippymodem_v34_mp_receiver *receiver, uint8_t bit,
	struct sippymodem_v34_mp_receive_event *event)
{
	struct sippymodem_v34_mp_receiver next;
	struct sippymodem_v34_mp_receive_event emitted = { 0 };

	if (!sippymodem_v34_mp_receiver_is_valid(receiver) || bit > 1u ||
	    event == NULL)
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	next = *receiver;
	receive_frame_bit(&next, bit, &emitted);
	if (!sippymodem_v34_mp_receiver_is_valid(&next))
		return SIPPYMODEM_V34_MP_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
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
