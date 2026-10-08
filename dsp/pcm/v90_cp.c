// SPDX-License-Identifier: BSD-3-Clause

#include "v90_cp.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CP_KIND_BIT = 19,
	CP_RATE_FIRST_BIT = 20,
	CP_RATE_BITS = 5,
	CP_SILENCE_BIT = 30,
	CP_UPSTREAM_RATE_FIRST_BIT = 36,
	CP_CODEC_DIFFERENCE_BIT = 128,
};

static bool kind_is_valid(enum v90_cp_kind kind)
{
	return kind == V90_CP_TRAINING || kind == V90_CP_DATA;
}

static bool constellations_are_unused(const struct v90_cp_parameters *parameters)
{
	return parameters->selected_rate_index == 0u ||
	       (parameters->kind == V90_CP_DATA && parameters->request_silence);
}

bool v90_cp_parameters_are_valid(
	const struct v90_cp_parameters *parameters)
{
	if (parameters == NULL || !kind_is_valid(parameters->kind) ||
	    !g711_law_is_valid(parameters->law) ||
	    parameters->selected_rate_index > V90_CP_RATE_INDEX_MAX ||
	    parameters->sign_redundancy > 3 ||
	    parameters->spectral_lookahead > 3 ||
	    (parameters->upstream_rate_mask & ~V90_CP_UPSTREAM_RATE_MASK) != 0)
		return false;
	if (constellations_are_unused(parameters)) {
		uint8_t declared_count = 1u;

		/* V.90 8.5.2/9.7 ignore CPs and cleardown constellations. Keep
		 * the bounds needed to frame and CRC-check their wire records. */
		if (parameters->constellation_count == 0u ||
		    parameters->constellation_count > V90_CP_MAX_CONSTELLATIONS)
			return false;
		for (size_t interval = 0; interval < V90_CP_FRAME_INTERVALS; ++interval) {
			if (parameters->frame_constellation[interval] >=
			    parameters->constellation_count)
				return false;
			if (parameters->frame_constellation[interval] >= declared_count)
				declared_count = parameters->frame_constellation[interval] + 1u;
		}
		return declared_count == parameters->constellation_count;
	}
	return pcm_constellations_are_canonical(
		       parameters->transmit_constellations,
		       parameters->codec_constellations,
		       parameters->frame_constellation,
		       parameters->constellation_count,
		       parameters->codec_constellations_differ);
}

bool v90_cp_parameters_are_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	if (!v90_cp_parameters_are_valid(left) ||
	    !v90_cp_parameters_are_valid(right) ||
	    left->kind != right->kind || left->law != right->law ||
	    left->upstream_rate_mask != right->upstream_rate_mask ||
	    left->trn1d_ratio_q3_13 != right->trn1d_ratio_q3_13 ||
	    left->shaping_a1_q1_6 != right->shaping_a1_q1_6 ||
	    left->shaping_a2_q1_6 != right->shaping_a2_q1_6 ||
	    left->shaping_b1_q1_6 != right->shaping_b1_q1_6 ||
	    left->shaping_b2_q1_6 != right->shaping_b2_q1_6 ||
	    left->selected_rate_index != right->selected_rate_index ||
	    left->sign_redundancy != right->sign_redundancy ||
	    left->spectral_lookahead != right->spectral_lookahead ||
	    left->constellation_count != right->constellation_count ||
	    left->request_silence != right->request_silence ||
	    left->acknowledges_peer != right->acknowledges_peer ||
	    left->codec_constellations_differ != right->codec_constellations_differ ||
	    memcmp(left->frame_constellation, right->frame_constellation,
		   sizeof(left->frame_constellation)) != 0)
		return false;
	for (size_t row = 0; row < left->constellation_count; ++row) {
		if (memcmp(left->transmit_constellations[row].ucode_mask,
			   right->transmit_constellations[row].ucode_mask,
			   V90_CP_CONSTELLATION_MASK_BYTES) != 0 ||
		    (left->codec_constellations_differ &&
		     memcmp(left->codec_constellations[row].ucode_mask,
			    right->codec_constellations[row].ucode_mask,
			    V90_CP_CONSTELLATION_MASK_BYTES) != 0))
			return false;
	}
	return true;
}

size_t v90_cp_frame_bits(const struct v90_cp_parameters *parameters)
{
	if (!v90_cp_parameters_are_valid(parameters))
		return 0;
	return pcm_control_frame_payload_end(parameters->constellation_count,
		parameters->codec_constellations_differ) + V90_CP_TRAILER_BITS;
}

size_t v90_cp_frame_bytes(const struct v90_cp_parameters *parameters)
{
	size_t bits = v90_cp_frame_bits(parameters);

	return bits == 0 ? 0 : (bits + 7u) / 8u;
}

static void encode_header(
	uint8_t *frame, const struct v90_cp_parameters *parameters)
{
	pcm_control_frame_write_prefix(frame);
	packed_msb_set_bit(
		frame, CP_KIND_BIT, parameters->kind == V90_CP_DATA);
	packed_msb_write_lsb_field(
		frame, CP_RATE_FIRST_BIT, CP_RATE_BITS,
		parameters->selected_rate_index);
	packed_msb_set_bit(
		frame, CP_SILENCE_BIT, parameters->request_silence);
	pcm_control_frame_write_options(frame,
		parameters->sign_redundancy, parameters->acknowledges_peer,
		parameters->law, parameters->spectral_lookahead);
	packed_msb_write_lsb_field(
		frame, CP_UPSTREAM_RATE_FIRST_BIT,
		V90_CP_UPSTREAM_RATE_BITS, parameters->upstream_rate_mask);
	pcm_control_frame_write_shaping(
		frame, parameters->trn1d_ratio_q3_13,
		parameters->shaping_a1_q1_6, parameters->shaping_a2_q1_6,
		parameters->shaping_b1_q1_6, parameters->shaping_b2_q1_6);
	pcm_control_frame_write_map(frame, parameters->frame_constellation);
	packed_msb_set_bit(
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

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V90_CP_INVALID_ARGUMENT;
	if (bit_count == 0)
		return V90_CP_INVALID_PARAMETERS;
	if (frame_capacity < byte_count)
		return V90_CP_INVALID_ARGUMENT;
	encode_header(encoded, parameters);
	pcm_control_frame_write_constellation_payload(
		encoded, parameters->transmit_constellations,
		parameters->codec_constellations, parameters->constellation_count,
		parameters->codec_constellations_differ);
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V90_CP_OK;
}

static bool framing_is_valid(
	const uint8_t *frame, size_t payload_end,
	size_t frame_bit_count)
{
	/* Table 14's reserved information bits are not interpreted by the
	 * receiver. Keep them in the CRC, but only validate wire framing here. */
	return pcm_control_frame_prefix_is_valid(frame) &&
	       pcm_control_frame_starts_are_zero(frame, 34u, payload_end + 1u) &&
	       packed_msb_bits_are_zero(frame, payload_end + 17u, 3) &&
	       packed_msb_bits_are_zero(
		       frame, frame_bit_count,
		       (8u - frame_bit_count % 8u) % 8u);
}

static void decode_header(
	const uint8_t *frame, struct v90_cp_parameters *parameters)
{
	parameters->kind = packed_msb_bit(frame, CP_KIND_BIT) != 0 ?
				   V90_CP_DATA : V90_CP_TRAINING;
	parameters->selected_rate_index =
		(uint8_t)packed_msb_read_lsb_field(
			frame, CP_RATE_FIRST_BIT, CP_RATE_BITS);
	parameters->request_silence =
		packed_msb_bit(frame, CP_SILENCE_BIT) != 0;
	parameters->upstream_rate_mask =
		(uint16_t)packed_msb_read_lsb_field(
			frame, CP_UPSTREAM_RATE_FIRST_BIT,
			V90_CP_UPSTREAM_RATE_BITS);
	pcm_control_frame_read_options(frame,
		&parameters->sign_redundancy, &parameters->acknowledges_peer,
		&parameters->law, &parameters->spectral_lookahead);
	pcm_control_frame_read_shaping(frame,
		&parameters->trn1d_ratio_q3_13,
		&parameters->shaping_a1_q1_6, &parameters->shaping_a2_q1_6,
		&parameters->shaping_b1_q1_6, &parameters->shaping_b2_q1_6);
}

enum v90_cp_result v90_cp_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count,
	struct v90_cp_parameters *parameters)
{
	struct v90_cp_parameters decoded = { 0 };
	size_t byte_count = (frame_bit_count + 7u) / 8u;
	size_t constellation_count;
	size_t expected_bits;
	size_t payload_end;

	if (frame == NULL || parameters == NULL ||
	    frame_bit_count < V90_CP_MIN_FRAME_BITS ||
	    frame_bit_count > V90_CP_MAX_FRAME_BITS ||
	    frame_capacity < byte_count)
		return V90_CP_INVALID_ARGUMENT;
	constellation_count = pcm_control_frame_read_map(
		frame, decoded.frame_constellation);
	if (constellation_count == 0)
		return V90_CP_INVALID_FRAME;
	decoded.codec_constellations_differ =
		packed_msb_bit(frame, CP_CODEC_DIFFERENCE_BIT) != 0;
	payload_end = pcm_control_frame_payload_end(
		constellation_count, decoded.codec_constellations_differ);
	expected_bits = payload_end + V90_CP_TRAILER_BITS;
	if (frame_bit_count != expected_bits)
		return V90_CP_INVALID_FRAME;
	if (!framing_is_valid(frame, payload_end, frame_bit_count))
		return V90_CP_INVALID_FRAME;
	if (!pcm_control_frame_read_constellation_payload(
		    frame, decoded.transmit_constellations,
		    decoded.codec_constellations, constellation_count,
		    decoded.codec_constellations_differ))
		return V90_CP_CRC_MISMATCH;
	decode_header(frame, &decoded);
	decoded.constellation_count = (uint8_t)constellation_count;
	if (!constellations_are_unused(&decoded) &&
	    !pcm_constellations_compact(
		    decoded.transmit_constellations, decoded.codec_constellations,
		    decoded.frame_constellation, &decoded.constellation_count,
		    decoded.codec_constellations_differ))
		return V90_CP_INVALID_FRAME;
	if (!v90_cp_parameters_are_valid(&decoded))
		return V90_CP_INVALID_FRAME;
	*parameters = decoded;
	return V90_CP_OK;
}
