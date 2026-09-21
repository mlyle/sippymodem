// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpt.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <limits.h>
#include <string.h>

enum {
	CPT_RATE_FIRST_BIT = 21,
	CPT_RATE_BITS = 5,
	CPT_SIGN_REDUNDANCY_FIRST_BIT = 31,
	CPT_SIGN_REDUNDANCY_BITS = 2,
	CPT_ACKNOWLEDGMENT_BIT = 33,
	CPT_LAW_BIT = 35,
	CPT_LOOKAHEAD_FIRST_BIT = 49,
	CPT_LOOKAHEAD_BITS = 2,
	CPT_RATIO_FIRST_BIT = 52,
	CPT_RATIO_BITS = 16,
	CPT_A1_FIRST_BIT = 69,
	CPT_A2_FIRST_BIT = 77,
	CPT_B1_FIRST_BIT = 86,
	CPT_B2_FIRST_BIT = 94,
	CPT_COEFFICIENT_BITS = 8,
	CPT_MAP_FIRST_BIT = 103,
	CPT_MAP_BITS = 4,
	CPT_CODEC_DIFFERENCE_BIT = 128,
	CPT_MASKS_FIRST_BIT = V92_CPT_HEADER_BITS,
	CPT_CRC_BITS = 16,
};

bool v92_cpt_parameters_are_valid(
	const struct v92_cpt_parameters *parameters)
{
	return parameters != NULL && g711_law_is_valid(parameters->law) &&
	       parameters->selected_rate_index <= V92_CPT_RATE_INDEX_MAX &&
	       parameters->sign_redundancy <= 3 &&
	       parameters->spectral_lookahead <= 3 &&
	       pcm_constellations_are_canonical(
		       parameters->transmit_constellations,
		       parameters->codec_constellations,
		       parameters->frame_constellation,
		       parameters->constellation_count,
		       parameters->codec_constellations_differ);
}

static size_t align_frame(size_t bit_count)
{
	return (bit_count + V92_CPT_SYMBOLS_PER_FRAME - 1u) /
	       V92_CPT_SYMBOLS_PER_FRAME * V92_CPT_SYMBOLS_PER_FRAME;
}

size_t v92_cpt_frame_bits(
	const struct v92_cpt_parameters *parameters)
{
	size_t constellation_sets;
	size_t unfilled;

	if (!v92_cpt_parameters_are_valid(parameters))
		return 0;
	constellation_sets = parameters->codec_constellations_differ ? 2u : 1u;
	unfilled = V92_CPT_HEADER_BITS +
		   constellation_sets * parameters->constellation_count *
			   V92_CPT_CONSTELLATION_BITS +
		   V92_CPT_MIN_TRAILER_BITS;
	return align_frame(unfilled);
}

size_t v92_cpt_frame_bytes(
	const struct v92_cpt_parameters *parameters)
{
	size_t bits = v92_cpt_frame_bits(parameters);

	return bits == 0 ? 0 : (bits + 7u) / 8u;
}

static size_t map_first_bit(size_t interval)
{
	return CPT_MAP_FIRST_BIT + interval * CPT_MAP_BITS +
	       (interval >= 4u ? 1u : 0u);
}

static size_t mask_bit(size_t set_index, uint8_t ucode)
{
	return CPT_MASKS_FIRST_BIT +
	       set_index * V92_CPT_CONSTELLATION_BITS +
	       (size_t)(ucode / 16u) * 17u + 1u + ucode % 16u;
}

static void encode_constellation(
	uint8_t *frame, size_t set_index,
	const struct pcm_constellation *constellation)
{
	for (uint8_t ucode = 0; ucode < PCM_CONSTELLATION_UCODES; ++ucode) {
		packed_msb_set_bit(
			frame, mask_bit(set_index, ucode),
			pcm_constellation_contains(constellation, ucode));
	}
}

static void encode_header(
	uint8_t *frame, const struct v92_cpt_parameters *parameters)
{
	for (size_t index = 0; index < 17u; ++index)
		packed_msb_set_bit(frame, index, true);
	packed_msb_write_lsb_field(
		frame, CPT_RATE_FIRST_BIT, CPT_RATE_BITS,
		parameters->selected_rate_index);
	packed_msb_write_lsb_field(
		frame, CPT_SIGN_REDUNDANCY_FIRST_BIT,
		CPT_SIGN_REDUNDANCY_BITS, parameters->sign_redundancy);
	packed_msb_set_bit(
		frame, CPT_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_peer);
	packed_msb_set_bit(
		frame, CPT_LAW_BIT, parameters->law == G711_LAW_PCMA);
	packed_msb_write_lsb_field(
		frame, CPT_LOOKAHEAD_FIRST_BIT, CPT_LOOKAHEAD_BITS,
		parameters->spectral_lookahead);
	packed_msb_write_lsb_field(
		frame, CPT_RATIO_FIRST_BIT, CPT_RATIO_BITS,
		parameters->trn1d_ratio_q3_13);
	packed_msb_write_lsb_field(
		frame, CPT_A1_FIRST_BIT, CPT_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_a1_q1_6);
	packed_msb_write_lsb_field(
		frame, CPT_A2_FIRST_BIT, CPT_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_a2_q1_6);
	packed_msb_write_lsb_field(
		frame, CPT_B1_FIRST_BIT, CPT_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_b1_q1_6);
	packed_msb_write_lsb_field(
		frame, CPT_B2_FIRST_BIT, CPT_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_b2_q1_6);
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval) {
		packed_msb_write_lsb_field(
			frame, map_first_bit(interval), CPT_MAP_BITS,
			parameters->frame_constellation[interval]);
	}
	packed_msb_set_bit(
		frame, CPT_CODEC_DIFFERENCE_BIT,
		parameters->codec_constellations_differ);
}

enum v92_cpt_result v92_cpt_encode(
	const struct v92_cpt_parameters *parameters,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	uint8_t encoded[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t bit_count = v92_cpt_frame_bits(parameters);
	size_t byte_count = v92_cpt_frame_bytes(parameters);
	size_t set_count;
	size_t payload_end;
	uint16_t crc;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_CPT_INVALID_ARGUMENT;
	if (bit_count == 0)
		return V92_CPT_INVALID_PARAMETERS;
	if (frame_capacity < byte_count)
		return V92_CPT_OUTPUT_TOO_SMALL;
	encode_header(encoded, parameters);
	set_count = parameters->constellation_count;
	for (size_t index = 0;
	     index < parameters->constellation_count; ++index) {
		encode_constellation(
			encoded, index,
			&parameters->transmit_constellations[index]);
	}
	if (parameters->codec_constellations_differ) {
		for (size_t index = 0;
		     index < parameters->constellation_count; ++index) {
			encode_constellation(
				encoded, set_count + index,
				&parameters->codec_constellations[index]);
		}
		set_count += parameters->constellation_count;
	}
	payload_end = V92_CPT_HEADER_BITS +
		      set_count * V92_CPT_CONSTELLATION_BITS;
	crc = pcm_control_frame_crc(encoded, payload_end);
	packed_msb_write_msb_field(
		encoded, payload_end + 1u, CPT_CRC_BITS, crc);
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V92_CPT_OK;
}

static bool framing_is_valid(
	const uint8_t *frame, size_t payload_end,
	size_t frame_bit_count)
{
	if (packed_msb_read_msb_field(frame, 0u, 17u) != UINT32_C(0x1ffff))
		return false;
	for (size_t index = 17u; index <= payload_end; index += 17u) {
		if (packed_msb_bit(frame, index) != 0)
			return false;
	}
	return packed_msb_bits_are_zero(frame, 18u, 3u) &&
	       packed_msb_bits_are_zero(
		       frame, payload_end + 1u + CPT_CRC_BITS,
		       frame_bit_count - payload_end - 1u - CPT_CRC_BITS) &&
	       packed_msb_bits_are_zero(
		       frame, frame_bit_count,
		       (8u - frame_bit_count % 8u) % 8u);
}

static int8_t decode_signed_q1_6(
	const uint8_t *frame, size_t first_bit)
{
	uint8_t raw = (uint8_t)packed_msb_read_lsb_field(
		frame, first_bit, CPT_COEFFICIENT_BITS);

	return raw <= INT8_MAX ? (int8_t)raw :
	       (int8_t)(-1 - (int16_t)(UINT8_MAX - raw));
}

static void decode_constellation(
	const uint8_t *frame, size_t set_index,
	struct pcm_constellation *constellation)
{
	for (uint8_t ucode = 0; ucode < PCM_CONSTELLATION_UCODES; ++ucode) {
		if (packed_msb_bit(frame, mask_bit(set_index, ucode)) != 0)
			constellation->ucode_mask[ucode / 8u] |=
				(uint8_t)(1u << (ucode % 8u));
	}
}

static void decode_header(
	const uint8_t *frame, struct v92_cpt_parameters *parameters)
{
	parameters->law = packed_msb_bit(frame, CPT_LAW_BIT) != 0 ?
				  G711_LAW_PCMA : G711_LAW_PCMU;
	parameters->selected_rate_index =
		(uint8_t)packed_msb_read_lsb_field(
			frame, CPT_RATE_FIRST_BIT, CPT_RATE_BITS);
	parameters->sign_redundancy =
		(uint8_t)packed_msb_read_lsb_field(
			frame, CPT_SIGN_REDUNDANCY_FIRST_BIT,
			CPT_SIGN_REDUNDANCY_BITS);
	parameters->acknowledges_peer =
		packed_msb_bit(frame, CPT_ACKNOWLEDGMENT_BIT) != 0;
	parameters->spectral_lookahead =
		(uint8_t)packed_msb_read_lsb_field(
			frame, CPT_LOOKAHEAD_FIRST_BIT, CPT_LOOKAHEAD_BITS);
	parameters->trn1d_ratio_q3_13 =
		(uint16_t)packed_msb_read_lsb_field(
			frame, CPT_RATIO_FIRST_BIT, CPT_RATIO_BITS);
	parameters->shaping_a1_q1_6 =
		decode_signed_q1_6(frame, CPT_A1_FIRST_BIT);
	parameters->shaping_a2_q1_6 =
		decode_signed_q1_6(frame, CPT_A2_FIRST_BIT);
	parameters->shaping_b1_q1_6 =
		decode_signed_q1_6(frame, CPT_B1_FIRST_BIT);
	parameters->shaping_b2_q1_6 =
		decode_signed_q1_6(frame, CPT_B2_FIRST_BIT);
}

enum v92_cpt_result v92_cpt_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count,
	struct v92_cpt_parameters *parameters)
{
	struct v92_cpt_parameters decoded = { 0 };
	size_t constellation_count = 1;
	size_t set_count;
	size_t expected_bits;
	size_t payload_end;
	uint16_t received_crc = 0;

	if (frame == NULL || parameters == NULL)
		return V92_CPT_INVALID_ARGUMENT;
	if (frame_bit_count < V92_CPT_MIN_FRAME_BITS ||
	    frame_bit_count > V92_CPT_MAX_FRAME_BITS ||
	    frame_bit_count % V92_CPT_SYMBOLS_PER_FRAME != 0)
		return V92_CPT_INVALID_FRAME;
	if (frame_capacity < (frame_bit_count + 7u) / 8u)
		return V92_CPT_INVALID_ARGUMENT;
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval) {
		uint8_t index = (uint8_t)packed_msb_read_lsb_field(
			frame, map_first_bit(interval), CPT_MAP_BITS);

		if (index >= V92_CPT_MAX_CONSTELLATIONS)
			return V92_CPT_INVALID_FRAME;
		decoded.frame_constellation[interval] = index;
		if ((size_t)index + 1u > constellation_count)
			constellation_count = (size_t)index + 1u;
	}
	decoded.codec_constellations_differ =
		packed_msb_bit(frame, CPT_CODEC_DIFFERENCE_BIT) != 0;
	set_count = constellation_count *
		(decoded.codec_constellations_differ ? 2u : 1u);
	payload_end = V92_CPT_HEADER_BITS +
		      set_count * V92_CPT_CONSTELLATION_BITS;
	expected_bits = align_frame(
		payload_end + V92_CPT_MIN_TRAILER_BITS);
	if (frame_bit_count != expected_bits ||
	    !framing_is_valid(frame, payload_end, frame_bit_count))
		return V92_CPT_INVALID_FRAME;
	received_crc = (uint16_t)packed_msb_read_msb_field(
		frame, payload_end + 1u, CPT_CRC_BITS);
	if (received_crc != pcm_control_frame_crc(frame, payload_end))
		return V92_CPT_CRC_MISMATCH;

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
	if (!v92_cpt_parameters_are_valid(&decoded))
		return V92_CPT_INVALID_FRAME;
	*parameters = decoded;
	return V92_CPT_OK;
}

const char *v92_cpt_result_name(enum v92_cpt_result result)
{
	switch (result) {
	case V92_CPT_OK:
		return "ok";
	case V92_CPT_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_CPT_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_CPT_OUTPUT_TOO_SMALL:
		return "output too small";
	case V92_CPT_INVALID_FRAME:
		return "invalid frame";
	case V92_CPT_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
