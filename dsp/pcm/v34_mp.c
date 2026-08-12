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
	enum v34_trellis_selection trellis)
{
	return trellis == V34_TRELLIS_16_STATE ||
	       trellis == V34_TRELLIS_32_STATE ||
	       trellis == V34_TRELLIS_64_STATE;
}

static bool rate_is_valid(uint8_t rate_2400_bps_units)
{
	return rate_2400_bps_units >=
		       V34_MP_MINIMUM_RATE_2400_BPS_UNITS &&
	       rate_2400_bps_units <=
		       V34_MP_MAXIMUM_RATE_2400_BPS_UNITS;
}

bool v34_mp_parameters_are_valid(
	const struct v34_mp_parameters *parameters)
{
	return parameters != NULL &&
	       rate_is_valid(
		       parameters->maximum_call_to_answer_rate_2400_bps_units) &&
	       rate_is_valid(
		       parameters->maximum_answer_to_call_rate_2400_bps_units) &&
	       trellis_is_valid(parameters->remote_transmitter_trellis) &&
	       (parameters->data_rate_capability_mask &
		~V34_MP_RATE_CAPABILITY_MASK) == 0;
}

bool v34_mp_type1_parameters_are_valid(
	const struct v34_mp_type1_parameters *parameters)
{
	if (parameters == NULL ||
	    !v34_mp_parameters_are_valid(&parameters->common))
		return false;
	for (size_t index = 0; index < V34_PRECODER_TAP_COUNT;
	     ++index) {
		if (parameters->precoder_coefficients[index].real == INT16_MIN ||
		    parameters->precoder_coefficients[index].imaginary ==
			    INT16_MIN)
			return false;
	}
	return true;
}

bool v34_mp_record_is_valid(const struct v34_mp_record *record)
{
	if (record == NULL)
		return false;
	if (record->type == V34_MP_RECORD_TYPE0)
		return v34_mp_parameters_are_valid(&record->parameters.type0);
	return record->type == V34_MP_RECORD_TYPE1 &&
	       v34_mp_type1_parameters_are_valid(&record->parameters.type1);
}

static const struct v34_mp_parameters *record_common(
	const struct v34_mp_record *record)
{
	return record->type == V34_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static uint8_t minimum_rate(uint8_t first, uint8_t second)
{
	return first < second ? first : second;
}

static uint8_t highest_enabled_rate(uint16_t enabled, uint8_t limit)
{
	while (limit != 0) {
		if ((enabled & (uint16_t)(1u << (limit - 1u))) != 0)
			return limit;
		--limit;
	}
	return 0;
}

enum v34_mp_rate_result v34_mp_select_rates(
	const struct v34_mp_record *first,
	const struct v34_mp_record *second,
	struct v34_mp_rates *rates)
{
	const struct v34_mp_parameters *first_common;
	const struct v34_mp_parameters *second_common;
	struct v34_mp_rates selected;
	uint16_t enabled;
	uint8_t call_limit;
	uint8_t answer_limit;

	if (!v34_mp_record_is_valid(first) ||
	    !v34_mp_record_is_valid(second) || rates == NULL)
		return V34_MP_RATE_INVALID_ARGUMENT;
	first_common = record_common(first);
	second_common = record_common(second);
	enabled = (uint16_t)(first_common->data_rate_capability_mask &
			     second_common->data_rate_capability_mask);
	call_limit = minimum_rate(
		first_common->maximum_call_to_answer_rate_2400_bps_units,
		second_common->maximum_call_to_answer_rate_2400_bps_units);
	answer_limit = minimum_rate(
		first_common->maximum_answer_to_call_rate_2400_bps_units,
		second_common->maximum_answer_to_call_rate_2400_bps_units);
	selected = (struct v34_mp_rates){
		.auxiliary_channel_enabled =
			first_common->auxiliary_channel_enabled &&
			second_common->auxiliary_channel_enabled,
		.asymmetric_rates_enabled =
			first_common->asymmetric_rates_enabled &&
			second_common->asymmetric_rates_enabled,
	};
	if (!selected.asymmetric_rates_enabled) {
		uint8_t symmetric_limit =
			minimum_rate(call_limit, answer_limit);

		selected.call_to_answer_2400_bps_units =
			highest_enabled_rate(enabled, symmetric_limit);
		selected.answer_to_call_2400_bps_units =
			selected.call_to_answer_2400_bps_units;
	} else {
		selected.call_to_answer_2400_bps_units =
			highest_enabled_rate(enabled, call_limit);
		selected.answer_to_call_2400_bps_units =
			highest_enabled_rate(enabled, answer_limit);
	}
	if (selected.call_to_answer_2400_bps_units == 0 ||
	    selected.answer_to_call_2400_bps_units == 0)
		return V34_MP_RATE_NO_COMMON_RATE;
	*rates = selected;
	return V34_MP_RATE_OK;
}

const char *v34_mp_rate_result_name(enum v34_mp_rate_result result)
{
	switch (result) {
	case V34_MP_RATE_OK:
		return "ok";
	case V34_MP_RATE_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_MP_RATE_NO_COMMON_RATE:
		return "no common rate";
	}
	return "unknown";
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
		CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = V34_MP_TYPE_BIT; index < crc_first_bit; ++index) {
		if (!is_start_bit(index))
			remainder = crc16_ccitt_false_step(
				remainder, frame_bit(frame, index) != 0);
	}
	return remainder;
}

static void encode_common_parameters(
	uint8_t *frame, const struct v34_mp_parameters *parameters,
	bool type1)
{
	for (size_t index = V34_MP_FRAME_SYNC_FIRST_BIT;
	     index < V34_MP_TYPE0_FRAME_SYNC_BITS; ++index)
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
		V34_MP_RATE_CAPABILITY_BITS,
		parameters->data_rate_capability_mask);
	set_frame_bit(
		frame, V34_MP_ASYMMETRIC_RATES_BIT,
		parameters->asymmetric_rates_enabled);
}

static void decode_common_parameters(
	const uint8_t *frame, struct v34_mp_parameters *parameters)
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
		(enum v34_trellis_selection)decode_lsb_first_field(
			frame, V34_MP_TRELLIS_FIRST_BIT, V34_MP_TRELLIS_BITS);
	parameters->remote_transmitter_nonlinear_encoding =
		frame_bit(frame, V34_MP_NONLINEAR_ENCODING_BIT) != 0;
	parameters->remote_transmitter_expanded_shaping =
		frame_bit(frame, V34_MP_EXPANDED_SHAPING_BIT) != 0;
	parameters->acknowledges_peer_mp =
		frame_bit(frame, V34_MP_ACKNOWLEDGMENT_BIT) != 0;
	parameters->data_rate_capability_mask = decode_lsb_first_field(
		frame, V34_MP_RATE_CAPABILITY_FIRST_BIT,
		V34_MP_RATE_CAPABILITY_BITS);
	parameters->asymmetric_rates_enabled =
		frame_bit(frame, V34_MP_ASYMMETRIC_RATES_BIT) != 0;
}

enum v34_mp_result v34_mp_type0_encode(
	const struct v34_mp_parameters *parameters,
	uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[V34_MP_TYPE0_FRAME_BYTES] = { 0 };
	uint16_t crc;

	if (parameters == NULL || frame == NULL ||
	    frame_capacity < V34_MP_TYPE0_FRAME_BYTES)
		return V34_MP_INVALID_ARGUMENT;
	if (!v34_mp_parameters_are_valid(parameters))
		return V34_MP_INVALID_PARAMETERS;

	encode_common_parameters(encoded, parameters, false);

	crc = frame_crc(encoded, V34_MP_TYPE0_CRC_FIRST_BIT);
	/* The CCITT-FALSE numeric representation holds the first V.34 CRC
	 * output in its most-significant bit. */
	encode_msb_first_field(
		encoded, V34_MP_TYPE0_CRC_FIRST_BIT,
		V34_MP_CRC_BITS, crc);
	memcpy(frame, encoded, sizeof(encoded));
	return V34_MP_OK;
}

static bool frame_sync_is_valid(const uint8_t *frame)
{
	for (size_t index = V34_MP_FRAME_SYNC_FIRST_BIT;
	     index < V34_MP_TYPE0_FRAME_SYNC_BITS; ++index) {
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

enum v34_mp_result v34_mp_type0_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v34_mp_parameters *parameters)
{
	struct v34_mp_parameters decoded = { 0 };
	uint16_t received_crc;

	if (frame == NULL || parameters == NULL ||
	    frame_capacity < V34_MP_TYPE0_FRAME_BYTES)
		return V34_MP_INVALID_ARGUMENT;
	if (!framing_bits_are_valid(
		    frame, false, V34_MP_TYPE0_CRC_FIRST_BIT,
		    V34_MP_TYPE0_FILL_FIRST_BIT, V34_MP_TYPE0_FILL_BITS))
		return V34_MP_INVALID_FRAME;
	received_crc = decode_msb_first_field(
		frame, V34_MP_TYPE0_CRC_FIRST_BIT,
		V34_MP_CRC_BITS);
	if (received_crc != frame_crc(frame, V34_MP_TYPE0_CRC_FIRST_BIT))
		return V34_MP_CRC_MISMATCH;

	decode_common_parameters(frame, &decoded);
	if (!v34_mp_parameters_are_valid(&decoded))
		return V34_MP_INVALID_FRAME;
	*parameters = decoded;
	return V34_MP_OK;
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

enum v34_mp_result v34_mp_type1_encode(
	const struct v34_mp_type1_parameters *parameters,
	uint8_t *frame, size_t frame_capacity)
{
	uint8_t encoded[V34_MP_TYPE1_FRAME_BYTES] = { 0 };
	uint16_t crc;

	if (parameters == NULL || frame == NULL ||
	    frame_capacity < V34_MP_TYPE1_FRAME_BYTES)
		return V34_MP_INVALID_ARGUMENT;
	if (!v34_mp_type1_parameters_are_valid(parameters))
		return V34_MP_INVALID_PARAMETERS;

	encode_common_parameters(encoded, &parameters->common, true);
	for (size_t index = 0; index < V34_PRECODER_TAP_COUNT;
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
		V34_MP_CRC_BITS, crc);
	memcpy(frame, encoded, sizeof(encoded));
	return V34_MP_OK;
}

enum v34_mp_result v34_mp_type1_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v34_mp_type1_parameters *parameters)
{
	struct v34_mp_type1_parameters decoded = { 0 };
	uint16_t received_crc;

	if (frame == NULL || parameters == NULL ||
	    frame_capacity < V34_MP_TYPE1_FRAME_BYTES)
		return V34_MP_INVALID_ARGUMENT;
	if (!framing_bits_are_valid(
		    frame, true, V34_MP_TYPE1_CRC_FIRST_BIT,
		    V34_MP_TYPE1_FILL_FIRST_BIT, V34_MP_TYPE1_FILL_BITS))
		return V34_MP_INVALID_FRAME;
	received_crc = decode_msb_first_field(
		frame, V34_MP_TYPE1_CRC_FIRST_BIT,
		V34_MP_CRC_BITS);
	if (received_crc != frame_crc(frame, V34_MP_TYPE1_CRC_FIRST_BIT))
		return V34_MP_CRC_MISMATCH;

	decode_common_parameters(frame, &decoded.common);
	for (size_t index = 0; index < V34_PRECODER_TAP_COUNT;
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
	if (!v34_mp_type1_parameters_are_valid(&decoded))
		return V34_MP_INVALID_FRAME;
	*parameters = decoded;
	return V34_MP_OK;
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
	for (; byte_index < V34_MP_TYPE1_FRAME_BYTES;
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

static bool receive_stage_is_valid(enum v34_mp_receive_stage stage)
{
	return stage == V34_MP_SEARCH_SYNC ||
	       stage == V34_MP_RECEIVE_TYPE ||
	       stage == V34_MP_RECEIVE_FRAME;
}

bool v34_mp_receiver_is_valid(
	const struct v34_mp_receiver *receiver)
{
	if (receiver == NULL ||
	    receiver->sync_one_count > V34_MP_TYPE0_FRAME_SYNC_BITS ||
	    receiver->bit_index > V34_MP_TYPE1_FRAME_BITS ||
	    !receive_stage_is_valid(receiver->stage) ||
	    !packed_tail_is_zero(receiver->frame, receiver->bit_index))
		return false;
	switch (receiver->stage) {
	case V34_MP_SEARCH_SYNC:
		return receiver->frame_bit_count == 0 &&
		       receiver->bit_index == 0;
	case V34_MP_RECEIVE_TYPE:
		return receiver->frame_bit_count == 0 &&
		       receiver->bit_index == V34_MP_TYPE_BIT &&
		       receiver->sync_one_count == 0 &&
		       receive_prefix_is_valid(receiver->frame);
	case V34_MP_RECEIVE_FRAME:
		return (receiver->frame_bit_count ==
				V34_MP_TYPE0_FRAME_BITS ||
			receiver->frame_bit_count ==
				V34_MP_TYPE1_FRAME_BITS) &&
		       receiver->bit_index > V34_MP_TYPE_BIT &&
		       receiver->bit_index < receiver->frame_bit_count &&
		       receiver->sync_one_count == 0 &&
		       receive_prefix_is_valid(receiver->frame) &&
		       frame_bit(receiver->frame, V34_MP_TYPE_BIT) ==
			       (uint8_t)(receiver->frame_bit_count ==
					 V34_MP_TYPE1_FRAME_BITS);
	}
	return false;
}

enum v34_mp_result v34_mp_receiver_init(
	struct v34_mp_receiver *receiver)
{
	struct v34_mp_receiver initialized = {
		.stage = V34_MP_SEARCH_SYNC,
	};

	if (receiver == NULL ||
	    !v34_mp_receiver_is_valid(&initialized))
		return V34_MP_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_MP_OK;
}

static void clear_receive_frame(struct v34_mp_receiver *receiver)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->frame_bit_count = 0;
	receiver->bit_index = 0;
	receiver->sync_one_count = 0;
	receiver->stage = V34_MP_SEARCH_SYNC;
}

static void begin_receive_frame(struct v34_mp_receiver *receiver)
{
	clear_receive_frame(receiver);
	for (size_t index = 0;
	     index < V34_MP_TYPE0_FRAME_SYNC_BITS; ++index)
		set_frame_bit(receiver->frame, index, true);
	receiver->bit_index = V34_MP_TYPE_BIT;
	receiver->stage = V34_MP_RECEIVE_TYPE;
}

static void complete_receive_frame(
	struct v34_mp_receiver *receiver,
	struct v34_mp_receive_event *event)
{
	enum v34_mp_result result;

	if (receiver->frame_bit_count == V34_MP_TYPE0_FRAME_BITS) {
		struct v34_mp_parameters parameters;

		result = v34_mp_type0_decode(
			receiver->frame, sizeof(receiver->frame), &parameters);
		if (result == V34_MP_OK) {
			event->type = V34_MP_RECEIVE_TYPE0;
			event->parameters.type0 = parameters;
		}
	} else {
		struct v34_mp_type1_parameters parameters;

		result = v34_mp_type1_decode(
			receiver->frame, sizeof(receiver->frame), &parameters);
		if (result == V34_MP_OK) {
			event->type = V34_MP_RECEIVE_TYPE1;
			event->parameters.type1 = parameters;
		}
	}
	event->frame_result = result;
	if (result != V34_MP_OK)
		event->type = V34_MP_RECEIVE_REJECTED;
	clear_receive_frame(receiver);
}

static void receive_frame_bit(
	struct v34_mp_receiver *receiver, uint8_t bit,
	struct v34_mp_receive_event *event)
{
	if (receiver->stage == V34_MP_SEARCH_SYNC) {
		if (bit != 0) {
			if (receiver->sync_one_count <
			    V34_MP_TYPE0_FRAME_SYNC_BITS)
				receiver->sync_one_count++;
		} else if (receiver->sync_one_count ==
			   V34_MP_TYPE0_FRAME_SYNC_BITS) {
			begin_receive_frame(receiver);
		} else {
			receiver->sync_one_count = 0;
		}
		return;
	}

	set_frame_bit(receiver->frame, receiver->bit_index, bit != 0);
	receiver->bit_index++;
	if (receiver->stage == V34_MP_RECEIVE_TYPE) {
		receiver->frame_bit_count = bit != 0 ?
			V34_MP_TYPE1_FRAME_BITS :
			V34_MP_TYPE0_FRAME_BITS;
		receiver->stage = V34_MP_RECEIVE_FRAME;
	}
	if (receiver->bit_index == receiver->frame_bit_count)
		complete_receive_frame(receiver, event);
}

enum v34_mp_result v34_mp_receive_bit(
	struct v34_mp_receiver *receiver, uint8_t bit,
	struct v34_mp_receive_event *event)
{
	struct v34_mp_receiver next;
	struct v34_mp_receive_event emitted = { 0 };

	if (!v34_mp_receiver_is_valid(receiver) || bit > 1u ||
	    event == NULL)
		return V34_MP_INVALID_ARGUMENT;
	next = *receiver;
	receive_frame_bit(&next, bit, &emitted);
	if (!v34_mp_receiver_is_valid(&next))
		return V34_MP_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V34_MP_OK;
}

const char *v34_mp_result_name(
	enum v34_mp_result result)
{
	switch (result) {
	case V34_MP_OK:
		return "ok";
	case V34_MP_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_MP_INVALID_PARAMETERS:
		return "invalid parameters";
	case V34_MP_INVALID_FRAME:
		return "invalid frame";
	case V34_MP_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
