// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpt.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CPT_RATE_FIRST_BIT = 21,
	CPT_RATE_BITS = 5,
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

size_t v92_cpt_frame_bits(
	const struct v92_cpt_parameters *parameters)
{
	if (!v92_cpt_parameters_are_valid(parameters))
		return 0;
	return pcm_control_frame_constellation_bits(parameters->constellation_count,
		parameters->codec_constellations_differ, V92_CPT_SYMBOLS_PER_FRAME);
}

size_t v92_cpt_frame_bytes(
	const struct v92_cpt_parameters *parameters)
{
	return (v92_cpt_frame_bits(parameters) + 7u) / 8u;
}

static void encode_header(
	uint8_t *frame, const struct v92_cpt_parameters *parameters)
{
	pcm_control_frame_write_prefix(frame);
	packed_msb_write_lsb_field(
		frame, CPT_RATE_FIRST_BIT, CPT_RATE_BITS,
		parameters->selected_rate_index);
	pcm_control_frame_write_options(frame,
		parameters->sign_redundancy, parameters->acknowledges_peer,
		parameters->law, parameters->spectral_lookahead);
	pcm_control_frame_write_shaping(
		frame, parameters->trn1d_ratio_q3_13,
		parameters->shaping_a1_q1_6, parameters->shaping_a2_q1_6,
		parameters->shaping_b1_q1_6, parameters->shaping_b2_q1_6);
	pcm_control_frame_write_map(frame, parameters->frame_constellation,
		parameters->codec_constellations_differ);
}

enum v92_cpt_result v92_cpt_encode(
	const struct v92_cpt_parameters *parameters,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	uint8_t encoded[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t bit_count = v92_cpt_frame_bits(parameters);
	size_t byte_count = (bit_count + 7u) / 8u;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_CPT_INVALID_ARGUMENT;
	if (bit_count == 0)
		return V92_CPT_INVALID_PARAMETERS;
	if (frame_capacity < byte_count)
		return V92_CPT_OUTPUT_TOO_SMALL;
	encode_header(encoded, parameters);
	pcm_control_frame_write_constellation_payload(
		encoded, parameters->transmit_constellations,
		parameters->codec_constellations, parameters->constellation_count,
		parameters->codec_constellations_differ);
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V92_CPT_OK;
}

static void decode_header(
	const uint8_t *frame, struct v92_cpt_parameters *parameters)
{
	parameters->selected_rate_index =
		(uint8_t)packed_msb_read_lsb_field(
			frame, CPT_RATE_FIRST_BIT, CPT_RATE_BITS);
	pcm_control_frame_read_options(frame,
		&parameters->sign_redundancy, &parameters->acknowledges_peer,
		&parameters->law, &parameters->spectral_lookahead);
	pcm_control_frame_read_shaping(frame,
		&parameters->trn1d_ratio_q3_13,
		&parameters->shaping_a1_q1_6, &parameters->shaping_a2_q1_6,
		&parameters->shaping_b1_q1_6, &parameters->shaping_b2_q1_6);
}

enum v92_cpt_result v92_cpt_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count,
	struct v92_cpt_parameters *parameters)
{
	struct v92_cpt_parameters decoded = { 0 };

	if (frame == NULL || parameters == NULL)
		return V92_CPT_INVALID_ARGUMENT;
	if (frame_bit_count < V92_CPT_MIN_FRAME_BITS ||
	    frame_bit_count > V92_CPT_MAX_FRAME_BITS ||
	    frame_bit_count % V92_CPT_SYMBOLS_PER_FRAME != 0)
		return V92_CPT_INVALID_FRAME;
	if (frame_capacity < (frame_bit_count + 7u) / 8u)
		return V92_CPT_INVALID_ARGUMENT;
	if (!packed_msb_bits_are_zero(frame, 18u, 3u))
		return V92_CPT_INVALID_FRAME;
	struct pcm_control_constellation_layout layout = pcm_control_frame_read_constellation_layout(
		frame, decoded.frame_constellation);
	enum pcm_control_constellation_result result = pcm_control_frame_decode_constellations(
		frame, frame_bit_count, V92_CPT_SYMBOLS_PER_FRAME, layout,
		decoded.transmit_constellations, decoded.codec_constellations);
	if (result != PCM_CONTROL_CONSTELLATION_OK)
		return result == PCM_CONTROL_CONSTELLATION_CRC_MISMATCH ?
			V92_CPT_CRC_MISMATCH : V92_CPT_INVALID_FRAME;
	decode_header(frame, &decoded);
	decoded.constellation_count = layout.rows;
	decoded.codec_constellations_differ = layout.codec_differs;
	if (!pcm_constellations_compact(
		    decoded.transmit_constellations, decoded.codec_constellations,
		    decoded.frame_constellation, &decoded.constellation_count,
		    decoded.codec_constellations_differ))
		return V92_CPT_INVALID_FRAME;
	if (!v92_cpt_parameters_are_valid(&decoded))
		return V92_CPT_INVALID_FRAME;
	*parameters = decoded;
	return V92_CPT_OK;
}
