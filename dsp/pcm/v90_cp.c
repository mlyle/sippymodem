// SPDX-License-Identifier: BSD-3-Clause

#include "v90_cp.h"

#include "crc16_ccitt_false.h"

#include <string.h>

enum {
	CP_START_FIRST_BIT = 17,
	CP_INFORMATION_FIRST_BIT = 18,
	CP_KIND_BIT = 19,
	CP_RATE_FIRST_BIT = 20,
	CP_RATE_BITS = 5,
	CP_SILENCE_BIT = 30,
	CP_SIGN_REDUNDANCY_FIRST_BIT = 31,
	CP_SIGN_REDUNDANCY_BITS = 2,
	CP_ACKNOWLEDGMENT_BIT = 33,
	CP_LAW_BIT = 35,
	CP_UPSTREAM_RATE_FIRST_BIT = 36,
	CP_LOOKAHEAD_FIRST_BIT = 49,
	CP_LOOKAHEAD_BITS = 2,
	CP_RATIO_FIRST_BIT = 52,
	CP_RATIO_BITS = 16,
	CP_A1_FIRST_BIT = 69,
	CP_A2_FIRST_BIT = 77,
	CP_B1_FIRST_BIT = 86,
	CP_B2_FIRST_BIT = 94,
	CP_COEFFICIENT_BITS = 8,
	CP_MAP_FIRST_BIT = 103,
	CP_MAP_BITS = 4,
	CP_CODEC_DIFFERENCE_BIT = 128,
	CP_MASKS_FIRST_BIT = V90_CP_HEADER_BITS,
	CP_CRC_BITS = 16,
};

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static bool kind_is_valid(enum v90_cp_kind kind)
{
	return kind == V90_CP_TRAINING || kind == V90_CP_DATA;
}

static bool constellation_is_empty(
	const struct pcm_constellation *constellation)
{
	for (size_t index = 0;
	     index < V90_CP_CONSTELLATION_MASK_BYTES; ++index) {
		if (constellation->ucode_mask[index] != 0)
			return false;
	}
	return true;
}

static bool constellations_are_canonical(
	const struct v90_cp_parameters *parameters)
{
	uint8_t used = 0;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t index = parameters->frame_constellation[interval];

		if (index >= parameters->constellation_count)
			return false;
		used |= (uint8_t)(1u << index);
	}
	if (used != (uint8_t)((1u << parameters->constellation_count) - 1u))
		return false;
	for (size_t index = 0;
	     index < V90_CP_MAX_CONSTELLATIONS; ++index) {
		bool active = index < parameters->constellation_count;
		bool transmit_empty = constellation_is_empty(
			&parameters->transmit_constellations[index]);
		bool codec_empty = constellation_is_empty(
			&parameters->codec_constellations[index]);

		if (active == transmit_empty)
			return false;
		if (parameters->codec_constellations_differ) {
			if (active == codec_empty)
				return false;
		} else if (!codec_empty) {
			return false;
		}
	}
	return true;
}

bool v90_cp_parameters_are_valid(
	const struct v90_cp_parameters *parameters)
{
	return parameters != NULL && kind_is_valid(parameters->kind) &&
	       law_is_valid(parameters->law) &&
	       parameters->selected_rate_index <= V90_CP_RATE_INDEX_MAX &&
	       parameters->sign_redundancy <= 3 &&
	       parameters->spectral_lookahead <= 3 &&
	       parameters->constellation_count >= 1 &&
	       parameters->constellation_count <=
		       V90_CP_MAX_CONSTELLATIONS &&
	       (parameters->upstream_rate_mask &
		~V90_CP_UPSTREAM_RATE_MASK) == 0 &&
	       constellations_are_canonical(parameters);
}

size_t v90_cp_frame_bits(const struct v90_cp_parameters *parameters)
{
	size_t constellation_sets;

	if (!v90_cp_parameters_are_valid(parameters))
		return 0;
	constellation_sets = parameters->codec_constellations_differ ? 2u : 1u;
	return V90_CP_HEADER_BITS +
	       constellation_sets * parameters->constellation_count *
		       V90_CP_CONSTELLATION_BITS +
	       V90_CP_TRAILER_BITS;
}

size_t v90_cp_frame_bytes(const struct v90_cp_parameters *parameters)
{
	size_t bits = v90_cp_frame_bits(parameters);

	return bits == 0 ? 0 : (bits + 7u) / 8u;
}

static uint8_t frame_bit(const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void set_frame_bit(
	uint8_t *frame, size_t bit_index, bool value)
{
	if (value)
		frame[bit_index / 8u] |=
			(uint8_t)(1u << (7u - bit_index % 8u));
}

static uint32_t decode_lsb_first(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	uint32_t value = 0;

	for (size_t index = 0; index < bit_count; ++index)
		value |= (uint32_t)frame_bit(frame, first_bit + index) << index;
	return value;
}

static void encode_lsb_first(
	uint8_t *frame, size_t first_bit, size_t bit_count,
	uint32_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		set_frame_bit(
			frame, first_bit + index,
			(value & (UINT32_C(1) << index)) != 0);
}

static size_t map_first_bit(size_t interval)
{
	return CP_MAP_FIRST_BIT + interval * CP_MAP_BITS +
	       (interval >= 4u ? 1u : 0u);
}

static size_t mask_bit(size_t set_index, uint8_t ucode)
{
	return CP_MASKS_FIRST_BIT + set_index * V90_CP_CONSTELLATION_BITS +
	       (size_t)(ucode / 16u) * 17u + 1u + ucode % 16u;
}

static uint16_t frame_crc(
	const uint8_t *frame, size_t payload_end)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t index = CP_INFORMATION_FIRST_BIT;
	     index < payload_end; ++index) {
		if (index % 17u != 0)
			remainder = crc16_ccitt_false_step(
				remainder, frame_bit(frame, index) != 0);
	}
	return remainder;
}

static void encode_constellation(
	uint8_t *frame, size_t set_index,
	const struct pcm_constellation *constellation)
{
	for (uint8_t ucode = 0;
	     ucode < V90_CP_CONSTELLATION_UCODES; ++ucode) {
		set_frame_bit(
			frame, mask_bit(set_index, ucode),
			pcm_constellation_contains(constellation, ucode));
	}
}

static void encode_header(
	uint8_t *frame, const struct v90_cp_parameters *parameters)
{
	for (size_t index = 0; index < 17u; ++index)
		set_frame_bit(frame, index, true);
	set_frame_bit(frame, CP_KIND_BIT, parameters->kind == V90_CP_DATA);
	encode_lsb_first(
		frame, CP_RATE_FIRST_BIT, CP_RATE_BITS,
		parameters->selected_rate_index);
	set_frame_bit(frame, CP_SILENCE_BIT, parameters->request_silence);
	encode_lsb_first(
		frame, CP_SIGN_REDUNDANCY_FIRST_BIT,
		CP_SIGN_REDUNDANCY_BITS, parameters->sign_redundancy);
	set_frame_bit(
		frame, CP_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_peer);
	set_frame_bit(frame, CP_LAW_BIT, parameters->law == G711_LAW_PCMA);
	encode_lsb_first(
		frame, CP_UPSTREAM_RATE_FIRST_BIT,
		V90_CP_UPSTREAM_RATE_BITS, parameters->upstream_rate_mask);
	encode_lsb_first(
		frame, CP_LOOKAHEAD_FIRST_BIT, CP_LOOKAHEAD_BITS,
		parameters->spectral_lookahead);
	encode_lsb_first(
		frame, CP_RATIO_FIRST_BIT, CP_RATIO_BITS,
		parameters->trn1d_ratio_q3_13);
	encode_lsb_first(
		frame, CP_A1_FIRST_BIT, CP_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_a1_q1_6);
	encode_lsb_first(
		frame, CP_A2_FIRST_BIT, CP_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_a2_q1_6);
	encode_lsb_first(
		frame, CP_B1_FIRST_BIT, CP_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_b1_q1_6);
	encode_lsb_first(
		frame, CP_B2_FIRST_BIT, CP_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_b2_q1_6);
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		encode_lsb_first(
			frame, map_first_bit(interval), CP_MAP_BITS,
			parameters->frame_constellation[interval]);
	}
	set_frame_bit(
		frame, CP_CODEC_DIFFERENCE_BIT,
		parameters->codec_constellations_differ);
}

enum v90_cp_result v90_cp_encode(
	const struct v90_cp_parameters *parameters,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	uint8_t encoded[V90_CP_MAX_FRAME_BYTES] = { 0 };
	size_t bit_count = v90_cp_frame_bits(parameters);
	size_t byte_count = v90_cp_frame_bytes(parameters);
	size_t set_count;
	size_t payload_end;
	uint16_t crc;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V90_CP_INVALID_ARGUMENT;
	if (bit_count == 0)
		return V90_CP_INVALID_PARAMETERS;
	if (frame_capacity < byte_count)
		return V90_CP_INVALID_ARGUMENT;
	encode_header(encoded, parameters);
	set_count = parameters->constellation_count;
	for (size_t index = 0;
	     index < parameters->constellation_count; ++index)
		encode_constellation(
			encoded, index,
			&parameters->transmit_constellations[index]);
	if (parameters->codec_constellations_differ) {
		for (size_t index = 0;
		     index < parameters->constellation_count; ++index)
			encode_constellation(
				encoded, set_count + index,
				&parameters->codec_constellations[index]);
		set_count += parameters->constellation_count;
	}
	payload_end = V90_CP_HEADER_BITS +
		      set_count * V90_CP_CONSTELLATION_BITS;
	crc = frame_crc(encoded, payload_end);
	for (size_t index = 0; index < CP_CRC_BITS; ++index) {
		set_frame_bit(
			encoded, payload_end + 1u + index,
			(crc & (uint16_t)(1u <<
				(CP_CRC_BITS - index - 1u))) != 0);
	}
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V90_CP_OK;
}

static bool bits_are_zero(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	for (size_t index = 0; index < bit_count; ++index) {
		if (frame_bit(frame, first_bit + index) != 0)
			return false;
	}
	return true;
}

static bool framing_is_valid(
	const uint8_t *frame, size_t payload_end,
	size_t frame_bit_count)
{
	for (size_t index = 0; index < 17u; ++index) {
		if (frame_bit(frame, index) == 0)
			return false;
	}
	for (size_t index = CP_START_FIRST_BIT;
	     index <= payload_end; index += 17u) {
		if (frame_bit(frame, index) != 0)
			return false;
	}
	return frame_bit(frame, 18) == 0 &&
	       bits_are_zero(frame, 25, 5) &&
	       bits_are_zero(frame, 129, 7) &&
	       bits_are_zero(frame, payload_end + 17u, 3) &&
	       bits_are_zero(
		       frame, frame_bit_count,
		       (8u - frame_bit_count % 8u) % 8u);
}

static int8_t decode_signed_q1_6(
	const uint8_t *frame, size_t first_bit)
{
	uint8_t raw = (uint8_t)decode_lsb_first(
		frame, first_bit, CP_COEFFICIENT_BITS);

	return raw <= INT8_MAX ? (int8_t)raw :
	       (int8_t)(-1 - (int16_t)(UINT8_MAX - raw));
}

static void decode_constellation(
	const uint8_t *frame, size_t set_index,
	struct pcm_constellation *constellation)
{
	for (uint8_t ucode = 0;
	     ucode < V90_CP_CONSTELLATION_UCODES; ++ucode) {
		if (frame_bit(frame, mask_bit(set_index, ucode)) != 0)
			constellation->ucode_mask[ucode / 8u] |=
				(uint8_t)(1u << (ucode % 8u));
	}
}

static void decode_header(
	const uint8_t *frame, struct v90_cp_parameters *parameters)
{
	parameters->kind = frame_bit(frame, CP_KIND_BIT) != 0 ?
				   V90_CP_DATA : V90_CP_TRAINING;
	parameters->law = frame_bit(frame, CP_LAW_BIT) != 0 ?
				  G711_LAW_PCMA : G711_LAW_PCMU;
	parameters->selected_rate_index = (uint8_t)decode_lsb_first(
		frame, CP_RATE_FIRST_BIT, CP_RATE_BITS);
	parameters->request_silence =
		frame_bit(frame, CP_SILENCE_BIT) != 0;
	parameters->sign_redundancy = (uint8_t)decode_lsb_first(
		frame, CP_SIGN_REDUNDANCY_FIRST_BIT,
		CP_SIGN_REDUNDANCY_BITS);
	parameters->acknowledges_peer =
		frame_bit(frame, CP_ACKNOWLEDGMENT_BIT) != 0;
	parameters->upstream_rate_mask = (uint16_t)decode_lsb_first(
		frame, CP_UPSTREAM_RATE_FIRST_BIT,
		V90_CP_UPSTREAM_RATE_BITS);
	parameters->spectral_lookahead = (uint8_t)decode_lsb_first(
		frame, CP_LOOKAHEAD_FIRST_BIT, CP_LOOKAHEAD_BITS);
	parameters->trn1d_ratio_q3_13 = (uint16_t)decode_lsb_first(
		frame, CP_RATIO_FIRST_BIT, CP_RATIO_BITS);
	parameters->shaping_a1_q1_6 =
		decode_signed_q1_6(frame, CP_A1_FIRST_BIT);
	parameters->shaping_a2_q1_6 =
		decode_signed_q1_6(frame, CP_A2_FIRST_BIT);
	parameters->shaping_b1_q1_6 =
		decode_signed_q1_6(frame, CP_B1_FIRST_BIT);
	parameters->shaping_b2_q1_6 =
		decode_signed_q1_6(frame, CP_B2_FIRST_BIT);
}

enum v90_cp_result v90_cp_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count,
	struct v90_cp_parameters *parameters)
{
	struct v90_cp_parameters decoded = { 0 };
	size_t byte_count = (frame_bit_count + 7u) / 8u;
	size_t constellation_count = 1;
	size_t set_count;
	size_t expected_bits;
	size_t payload_end;
	uint16_t received_crc = 0;

	if (frame == NULL || parameters == NULL ||
	    frame_bit_count < V90_CP_MIN_FRAME_BITS ||
	    frame_bit_count > V90_CP_MAX_FRAME_BITS ||
	    frame_capacity < byte_count)
		return V90_CP_INVALID_ARGUMENT;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t index = (uint8_t)decode_lsb_first(
			frame, map_first_bit(interval), CP_MAP_BITS);

		if (index >= V90_CP_MAX_CONSTELLATIONS)
			return V90_CP_INVALID_FRAME;
		decoded.frame_constellation[interval] = index;
		if ((size_t)index + 1u > constellation_count)
			constellation_count = (size_t)index + 1u;
	}
	decoded.codec_constellations_differ =
		frame_bit(frame, CP_CODEC_DIFFERENCE_BIT) != 0;
	set_count = constellation_count *
		    (decoded.codec_constellations_differ ? 2u : 1u);
	expected_bits = V90_CP_HEADER_BITS +
			set_count * V90_CP_CONSTELLATION_BITS +
			V90_CP_TRAILER_BITS;
	if (frame_bit_count != expected_bits)
		return V90_CP_INVALID_FRAME;
	payload_end = V90_CP_HEADER_BITS +
		      set_count * V90_CP_CONSTELLATION_BITS;
	if (!framing_is_valid(frame, payload_end, frame_bit_count))
		return V90_CP_INVALID_FRAME;
	for (size_t index = 0; index < CP_CRC_BITS; ++index)
		received_crc = (uint16_t)(
			(received_crc << 1) |
			frame_bit(frame, payload_end + 1u + index));
	if (received_crc != frame_crc(frame, payload_end))
		return V90_CP_CRC_MISMATCH;

	decode_header(frame, &decoded);
	decoded.constellation_count = (uint8_t)constellation_count;
	for (size_t index = 0; index < constellation_count; ++index)
		decode_constellation(
			frame, index, &decoded.transmit_constellations[index]);
	if (decoded.codec_constellations_differ) {
		for (size_t index = 0; index < constellation_count; ++index)
			decode_constellation(
				frame, constellation_count + index,
				&decoded.codec_constellations[index]);
	}
	if (!v90_cp_parameters_are_valid(&decoded))
		return V90_CP_INVALID_FRAME;
	*parameters = decoded;
	return V90_CP_OK;
}

const char *v90_cp_result_name(enum v90_cp_result result)
{
	switch (result) {
	case V90_CP_OK:
		return "ok";
	case V90_CP_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_CP_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_CP_INVALID_FRAME:
		return "invalid frame";
	case V90_CP_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
