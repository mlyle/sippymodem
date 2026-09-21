// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpu.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CPU_TYPE_FIRST_BIT = 19,
	CPU_TYPE_BITS = 2,
	CPU_RATE_FIRST_BIT = 21,
	CPU_RATE_BITS = 5,
	CPU_SIGN_REDUNDANCY_FIRST_BIT = 31,
	CPU_SIGN_REDUNDANCY_BITS = 2,
	CPU_ACKNOWLEDGMENT_BIT = 33,
	CPU_LAW_BIT = 35,
	CPU_LOOKAHEAD_FIRST_BIT = 49,
	CPU_LOOKAHEAD_BITS = 2,
	CPU_RATIO_FIRST_BIT = 52,
	CPU_RATIO_BITS = 16,
	CPU_A1_FIRST_BIT = 69,
	CPU_A2_FIRST_BIT = 77,
	CPU_B1_FIRST_BIT = 86,
	CPU_B2_FIRST_BIT = 94,
	CPU_COEFFICIENT_BITS = 8,
	CPU_MAP_FIRST_BIT = 103,
	CPU_MAP_BITS = 4,
	CPU_CODEC_DIFFERENCE_BIT = 128,
	CPU_MASKS_FIRST_BIT = V92_CPU_HEADER_BITS,
	CPU_CRC_BITS = 16,
	CPUS_INFORMATION_BITS = 34,
	CPUS_UNFILLED_BITS = 52,
};

bool v92_cpu_parameters_are_valid(
	const struct v92_cpu_parameters *parameters)
{
	return parameters != NULL && g711_law_is_valid(parameters->law) &&
	       parameters->data_rate_number <=
		       V92_CPU_MAX_DATA_RATE_NUMBER &&
	       parameters->sign_redundancy <= 3u &&
	       parameters->spectral_lookahead_frames <= 3u &&
	       pcm_constellations_are_canonical(
		       parameters->transmit_constellations,
		       parameters->codec_constellations,
		       parameters->frame_constellation,
		       parameters->constellation_count,
		       parameters->codec_constellations_differ);
}

static size_t frame_bits_for_shape(
	size_t constellation_count, bool codec_constellations_differ,
	unsigned int mapping_frame_bits)
{
	size_t sets = codec_constellations_differ ? 2u : 1u;
	size_t unfilled = V92_CPU_HEADER_BITS +
		sets * constellation_count *
		V92_CPU_CONSTELLATION_BITS +
		V92_CPU_MIN_TRAILER_BITS;
	size_t multiple = mapping_frame_bits;

	return (unfilled + multiple - 1u) / multiple * multiple;
}

size_t v92_cpu_frame_bits(
	const struct v92_cpu_parameters *parameters,
	unsigned int mapping_frame_bits)
{
	if (!v92_cpu_parameters_are_valid(parameters) ||
	    !v92_control_upstream_alignment_is_valid(mapping_frame_bits))
		return 0u;
	return frame_bits_for_shape(
		parameters->constellation_count,
		parameters->codec_constellations_differ,
		mapping_frame_bits);
}

size_t v92_cpu_frame_bytes(
	const struct v92_cpu_parameters *parameters,
	unsigned int mapping_frame_bits)
{
	size_t bits = v92_cpu_frame_bits(parameters, mapping_frame_bits);

	return bits == 0u ? 0u : (bits + 7u) / 8u;
}

static size_t map_first_bit(size_t interval)
{
	return CPU_MAP_FIRST_BIT + interval * CPU_MAP_BITS +
	       (interval >= 4u ? 1u : 0u);
}

static void encode_prefix(uint8_t *frame, unsigned int type)
{
	for (size_t bit = 0u; bit < 17u; ++bit)
		packed_msb_set_bit(frame, bit, true);
	packed_msb_write_lsb_field(
		frame, CPU_TYPE_FIRST_BIT, CPU_TYPE_BITS, type);
}

static void encode_header(
	uint8_t *frame, const struct v92_cpu_parameters *parameters)
{
	encode_prefix(frame, 1u);
	packed_msb_write_lsb_field(
		frame, CPU_RATE_FIRST_BIT, CPU_RATE_BITS,
		parameters->data_rate_number);
	packed_msb_write_lsb_field(
		frame, CPU_SIGN_REDUNDANCY_FIRST_BIT,
		CPU_SIGN_REDUNDANCY_BITS,
		parameters->sign_redundancy);
	packed_msb_set_bit(
		frame, CPU_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_cpd);
	packed_msb_set_bit(
		frame, CPU_LAW_BIT, parameters->law == G711_LAW_PCMA);
	packed_msb_write_lsb_field(
		frame, CPU_LOOKAHEAD_FIRST_BIT, CPU_LOOKAHEAD_BITS,
		parameters->spectral_lookahead_frames);
	packed_msb_write_lsb_field(
		frame, CPU_RATIO_FIRST_BIT, CPU_RATIO_BITS,
		parameters->trn1d_ratio_q3_13);
	packed_msb_write_lsb_field(
		frame, CPU_A1_FIRST_BIT, CPU_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_a1_q1_6);
	packed_msb_write_lsb_field(
		frame, CPU_A2_FIRST_BIT, CPU_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_a2_q1_6);
	packed_msb_write_lsb_field(
		frame, CPU_B1_FIRST_BIT, CPU_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_b1_q1_6);
	packed_msb_write_lsb_field(
		frame, CPU_B2_FIRST_BIT, CPU_COEFFICIENT_BITS,
		(uint8_t)parameters->shaping_b2_q1_6);
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		packed_msb_write_lsb_field(
			frame, map_first_bit(interval), CPU_MAP_BITS,
			parameters->frame_constellation[interval]);
	}
	packed_msb_set_bit(
		frame, CPU_CODEC_DIFFERENCE_BIT,
		parameters->codec_constellations_differ);
}

enum v92_cpu_result v92_cpu_encode(
	const struct v92_cpu_parameters *parameters,
	unsigned int mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	uint8_t encoded[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits;
	size_t frame_bytes;
	size_t sets;
	size_t payload_end;
	uint16_t crc;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_CPU_INVALID_ARGUMENT;
	frame_bits = v92_cpu_frame_bits(parameters, mapping_frame_bits);
	if (frame_bits == 0u)
		return V92_CPU_INVALID_PARAMETERS;
	frame_bytes = (frame_bits + 7u) / 8u;
	if (frame_capacity < frame_bytes)
		return V92_CPU_OUTPUT_TOO_SMALL;

	encode_header(encoded, parameters);
	pcm_control_frame_write_constellations(
		encoded, 0u, parameters->transmit_constellations,
		parameters->constellation_count);
	sets = parameters->codec_constellations_differ ? 2u : 1u;
	if (sets == 2u) {
		pcm_control_frame_write_constellations(
			encoded, parameters->constellation_count,
			parameters->codec_constellations,
			parameters->constellation_count);
	}
	payload_end = CPU_MASKS_FIRST_BIT + sets *
		parameters->constellation_count *
		V92_CPU_CONSTELLATION_BITS;
	crc = pcm_control_frame_crc(encoded, payload_end);
	packed_msb_write_msb_field(
		encoded, payload_end + 1u, CPU_CRC_BITS, crc);
	memcpy(frame, encoded, frame_bytes);
	*frame_bit_count = frame_bits;
	return V92_CPU_OK;
}

static bool prefix_is_valid(const uint8_t *frame, unsigned int type)
{
	return packed_msb_read_msb_field(frame, 0u, 17u) == UINT32_C(0x1ffff) &&
	       packed_msb_bit(frame, 17u) == 0u &&
	       packed_msb_bit(frame, 18u) == 0u &&
	       packed_msb_read_lsb_field(
		       frame, CPU_TYPE_FIRST_BIT, CPU_TYPE_BITS) == type;
}

size_t v92_cpus_frame_bits(unsigned int mapping_frame_bits)
{
	if (!v92_control_upstream_alignment_is_valid(mapping_frame_bits))
		return 0u;
	return (CPUS_UNFILLED_BITS + mapping_frame_bits - 1u) /
		mapping_frame_bits * mapping_frame_bits;
}

enum v92_cpu_result v92_cpus_encode(
	const struct v92_cpus_parameters *parameters,
	unsigned int mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity, size_t *frame_bit_count)
{
	uint8_t encoded[V92_CPUS_MAX_FRAME_BYTES] = { 0 };
	size_t bits = v92_cpus_frame_bits(mapping_frame_bits);
	size_t bytes = (bits + 7u) / 8u;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_CPU_INVALID_ARGUMENT;
	if (bits == 0u ||
	    parameters->data_rate_number > V92_CPU_MAX_DATA_RATE_NUMBER)
		return V92_CPU_INVALID_PARAMETERS;
	if (frame_capacity < bytes)
		return V92_CPU_OUTPUT_TOO_SMALL;
	encode_prefix(encoded, 2u);
	packed_msb_write_lsb_field(
		encoded, CPU_RATE_FIRST_BIT, CPU_RATE_BITS,
		parameters->data_rate_number);
	packed_msb_set_bit(
		encoded, CPU_ACKNOWLEDGMENT_BIT, parameters->acknowledges_cpd);
	packed_msb_write_msb_field(
		encoded, CPUS_INFORMATION_BITS + 1u, CPU_CRC_BITS,
		pcm_control_frame_crc(encoded, CPUS_INFORMATION_BITS));
	memcpy(frame, encoded, bytes);
	*frame_bit_count = bits;
	return V92_CPU_OK;
}

enum v92_cpu_result v92_cpus_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int mapping_frame_bits,
	struct v92_cpus_parameters *parameters)
{
	struct v92_cpus_parameters decoded;
	size_t bits = v92_cpus_frame_bits(mapping_frame_bits);
	size_t bytes = (bits + 7u) / 8u;

	if (frame == NULL || parameters == NULL)
		return V92_CPU_INVALID_ARGUMENT;
	if (bits == 0u || frame_bit_count != bits)
		return V92_CPU_INVALID_FRAME;
	if (frame_capacity < bytes)
		return V92_CPU_INVALID_ARGUMENT;
	if (!prefix_is_valid(frame, 2u) ||
	    packed_msb_bit(frame, CPUS_INFORMATION_BITS) != 0u ||
	    !packed_msb_bits_are_zero(
		    frame, CPUS_UNFILLED_BITS - 1u,
		    bits - CPUS_UNFILLED_BITS + 1u) ||
	    !packed_msb_tail_is_zero(frame, bits, bytes))
		return V92_CPU_INVALID_FRAME;
	if (packed_msb_read_msb_field(
		    frame, CPUS_INFORMATION_BITS + 1u, CPU_CRC_BITS) !=
	    pcm_control_frame_crc(frame, CPUS_INFORMATION_BITS))
		return V92_CPU_CRC_MISMATCH;
	decoded = (struct v92_cpus_parameters){
		.data_rate_number = (uint8_t)packed_msb_read_lsb_field(
			frame, CPU_RATE_FIRST_BIT, CPU_RATE_BITS),
		.acknowledges_cpd =
			packed_msb_bit(frame, CPU_ACKNOWLEDGMENT_BIT) != 0u,
	};
	if (decoded.data_rate_number > V92_CPU_MAX_DATA_RATE_NUMBER)
		return V92_CPU_INVALID_FRAME;
	*parameters = decoded;
	return V92_CPU_OK;
}

static bool header_framing_is_valid(const uint8_t *frame)
{
	return prefix_is_valid(frame, 1u) &&
	       packed_msb_bit(frame, 34u) == 0u &&
	       packed_msb_bit(frame, 51u) == 0u &&
	       packed_msb_bit(frame, 68u) == 0u &&
	       packed_msb_bit(frame, 85u) == 0u &&
	       packed_msb_bit(frame, 102u) == 0u &&
	       packed_msb_bit(frame, 119u) == 0u;
}

static bool decode_maps(
	const uint8_t *frame, struct v92_cpu_parameters *decoded)
{
	uint8_t used = 0u;
	uint8_t greatest = 0u;

	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		uint8_t index = (uint8_t)packed_msb_read_lsb_field(
			frame, map_first_bit(interval), CPU_MAP_BITS);

		if (index >= V92_CPU_MAX_CONSTELLATIONS)
			return false;
		decoded->frame_constellation[interval] = index;
		used |= (uint8_t)(1u << index);
		if (index > greatest)
			greatest = index;
	}
	decoded->constellation_count = (uint8_t)(greatest + 1u);
	return used == (uint8_t)((1u << decoded->constellation_count) - 1u);
}

static int8_t decode_signed_octet(uint8_t value)
{
	return value <= INT8_MAX ?
		(int8_t)value : (int8_t)((int)value - 256);
}

static bool decode_header(
	const uint8_t *frame, struct v92_cpu_parameters *decoded)
{
	*decoded = (struct v92_cpu_parameters){
		.law = packed_msb_bit(frame, CPU_LAW_BIT) != 0u ?
			G711_LAW_PCMA : G711_LAW_PCMU,
		.trn1d_ratio_q3_13 =
			(uint16_t)packed_msb_read_lsb_field(
				frame, CPU_RATIO_FIRST_BIT, CPU_RATIO_BITS),
		.shaping_a1_q1_6 = decode_signed_octet(
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_A1_FIRST_BIT, CPU_COEFFICIENT_BITS)),
		.shaping_a2_q1_6 = decode_signed_octet(
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_A2_FIRST_BIT, CPU_COEFFICIENT_BITS)),
		.shaping_b1_q1_6 = decode_signed_octet(
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_B1_FIRST_BIT, CPU_COEFFICIENT_BITS)),
		.shaping_b2_q1_6 = decode_signed_octet(
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_B2_FIRST_BIT, CPU_COEFFICIENT_BITS)),
		.data_rate_number =
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_RATE_FIRST_BIT, CPU_RATE_BITS),
		.sign_redundancy =
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_SIGN_REDUNDANCY_FIRST_BIT,
				CPU_SIGN_REDUNDANCY_BITS),
		.spectral_lookahead_frames =
			(uint8_t)packed_msb_read_lsb_field(
				frame, CPU_LOOKAHEAD_FIRST_BIT,
				CPU_LOOKAHEAD_BITS),
		.acknowledges_cpd =
			packed_msb_bit(frame, CPU_ACKNOWLEDGMENT_BIT) != 0u,
		.codec_constellations_differ =
			packed_msb_bit(frame, CPU_CODEC_DIFFERENCE_BIT) != 0u,
	};
	return decode_maps(frame, decoded);
}

enum v92_cpu_result v92_cpu_probe_frame_bits(
	const uint8_t *frame, size_t frame_capacity,
	size_t received_bit_count, unsigned int mapping_frame_bits,
	size_t *frame_bit_count)
{
	struct v92_cpu_parameters decoded;
	size_t inferred;

	if (frame == NULL || frame_bit_count == NULL)
		return V92_CPU_INVALID_ARGUMENT;
	if (!v92_control_upstream_alignment_is_valid(mapping_frame_bits) ||
	    received_bit_count > V92_CPU_MAX_FRAME_BITS)
		return V92_CPU_INVALID_FRAME;
	if (frame_capacity < (received_bit_count + 7u) / 8u)
		return V92_CPU_INVALID_ARGUMENT;
	if (received_bit_count >= CPUS_INFORMATION_BITS &&
	    packed_msb_read_lsb_field(
		    frame, CPU_TYPE_FIRST_BIT, CPU_TYPE_BITS) == 2u) {
		inferred = v92_cpus_frame_bits(mapping_frame_bits);
		if (!prefix_is_valid(frame, 2u) || inferred < received_bit_count ||
		    packed_msb_read_lsb_field(
			    frame, CPU_RATE_FIRST_BIT, CPU_RATE_BITS) >
			    V92_CPU_MAX_DATA_RATE_NUMBER)
			return V92_CPU_INVALID_FRAME;
		*frame_bit_count = inferred;
		return V92_CPU_OK;
	}
	if (received_bit_count < V92_CPU_HEADER_BITS)
		return V92_CPU_INCOMPLETE;
	if (!header_framing_is_valid(frame) ||
	    !decode_header(frame, &decoded) ||
	    decoded.data_rate_number > V92_CPU_MAX_DATA_RATE_NUMBER)
		return V92_CPU_INVALID_FRAME;
	inferred = frame_bits_for_shape(
		decoded.constellation_count,
		decoded.codec_constellations_differ, mapping_frame_bits);
	if (inferred < received_bit_count || inferred > V92_CPU_MAX_FRAME_BITS)
		return V92_CPU_INVALID_FRAME;
	*frame_bit_count = inferred;
	return V92_CPU_OK;
}

static bool row_starts_are_zero(
	const uint8_t *frame, size_t row_count)
{
	for (size_t row = 0u; row < row_count; ++row) {
		size_t first = CPU_MASKS_FIRST_BIT +
			row * V92_CPU_CONSTELLATION_BITS;

		for (size_t group = 0u; group < 8u; ++group) {
			if (packed_msb_bit(frame, first + group * 17u) != 0u)
				return false;
		}
	}
	return true;
}

enum v92_cpu_result v92_cpu_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int mapping_frame_bits,
	struct v92_cpu_parameters *parameters)
{
	struct v92_cpu_parameters decoded;
	size_t multiple;
	size_t frame_bytes;
	size_t sets;
	size_t row_count;
	size_t payload_end;
	uint16_t received_crc;

	if (frame == NULL || parameters == NULL)
		return V92_CPU_INVALID_ARGUMENT;
	if (!v92_control_upstream_alignment_is_valid(mapping_frame_bits) ||
	    frame_bit_count < V92_CPU_MIN_FRAME_BITS ||
	    frame_bit_count > V92_CPU_MAX_FRAME_BITS)
		return V92_CPU_INVALID_FRAME;
	multiple = mapping_frame_bits;
	if (frame_bit_count % multiple != 0u)
		return V92_CPU_INVALID_FRAME;
	frame_bytes = (frame_bit_count + 7u) / 8u;
	if (frame_capacity < frame_bytes)
		return V92_CPU_INVALID_ARGUMENT;
	if (!header_framing_is_valid(frame) || !decode_header(frame, &decoded))
		return V92_CPU_INVALID_FRAME;
	if (frame_bit_count != frame_bits_for_shape(
		    decoded.constellation_count,
		    decoded.codec_constellations_differ,
		    mapping_frame_bits))
		return V92_CPU_INVALID_FRAME;

	sets = decoded.codec_constellations_differ ? 2u : 1u;
	row_count = sets * decoded.constellation_count;
	payload_end = CPU_MASKS_FIRST_BIT +
		row_count * V92_CPU_CONSTELLATION_BITS;
	if (!row_starts_are_zero(frame, row_count) ||
	    packed_msb_bit(frame, payload_end) != 0u ||
	    !packed_msb_bits_are_zero(
		    frame, payload_end + 1u + CPU_CRC_BITS,
		    frame_bit_count - payload_end - 1u - CPU_CRC_BITS) ||
	    !packed_msb_tail_is_zero(
		    frame, frame_bit_count, frame_bytes))
		return V92_CPU_INVALID_FRAME;
	received_crc = (uint16_t)packed_msb_read_msb_field(
		frame, payload_end + 1u, CPU_CRC_BITS);
	if (received_crc != pcm_control_frame_crc(frame, payload_end))
		return V92_CPU_CRC_MISMATCH;

	pcm_control_frame_read_constellations(
		frame, 0u, decoded.transmit_constellations,
		decoded.constellation_count);
	if (sets == 2u) {
		pcm_control_frame_read_constellations(
			frame, decoded.constellation_count,
			decoded.codec_constellations,
			decoded.constellation_count);
	}
	if (!v92_cpu_parameters_are_valid(&decoded))
		return V92_CPU_INVALID_FRAME;
	*parameters = decoded;
	return V92_CPU_OK;
}

const char *v92_cpu_result_name(enum v92_cpu_result result)
{
	switch (result) {
	case V92_CPU_OK:
		return "ok";
	case V92_CPU_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_CPU_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_CPU_OUTPUT_TOO_SMALL:
		return "output too small";
	case V92_CPU_INVALID_FRAME:
		return "invalid frame";
	case V92_CPU_CRC_MISMATCH:
		return "CRC mismatch";
	case V92_CPU_INCOMPLETE:
		return "incomplete";
	}
	return "unknown";
}
