// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpu.h"

#include "crc16_ccitt_false.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CPU_INFORMATION_FIRST_BIT = 18,
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
};

static bool constellation_is_empty(
	const struct pcm_constellation *constellation)
{
	for (size_t byte = 0;
	     byte < PCM_CONSTELLATION_MASK_BYTES; ++byte) {
		if (constellation->ucode_mask[byte] != 0u)
			return false;
	}
	return true;
}

static bool constellations_are_canonical(
	const struct v92_cpu_parameters *parameters)
{
	uint8_t used = 0u;

	for (size_t interval = 0;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		uint8_t index = parameters->frame_constellation[interval];

		if (index >= parameters->constellation_count)
			return false;
		used |= (uint8_t)(1u << index);
	}
	if (used != (uint8_t)((1u << parameters->constellation_count) - 1u))
		return false;
	for (size_t index = 0;
	     index < V92_CPU_MAX_CONSTELLATIONS; ++index) {
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

bool v92_cpu_parameters_are_valid(
	const struct v92_cpu_parameters *parameters)
{
	return parameters != NULL && g711_law_is_valid(parameters->law) &&
	       parameters->data_rate_number <=
		       V92_CPU_MAX_DATA_RATE_NUMBER &&
	       parameters->sign_redundancy <= 3u &&
	       parameters->spectral_lookahead_frames <= 3u &&
	       parameters->constellation_count >= 1u &&
	       parameters->constellation_count <=
		       V92_CPU_MAX_CONSTELLATIONS &&
	       constellations_are_canonical(parameters);
}

static bool bits_per_symbol_is_valid(unsigned int bits_per_symbol)
{
	return bits_per_symbol >= V92_CPU_MIN_BITS_PER_SYMBOL &&
	       bits_per_symbol <= V92_CPU_MAX_BITS_PER_SYMBOL;
}

size_t v92_cpu_frame_bits(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol)
{
	size_t constellation_sets;
	size_t unfilled;
	size_t multiple;

	if (!v92_cpu_parameters_are_valid(parameters) ||
	    !bits_per_symbol_is_valid(bits_per_symbol))
		return 0u;
	constellation_sets = parameters->codec_constellations_differ ? 2u : 1u;
	unfilled = V92_CPU_HEADER_BITS +
		   constellation_sets * parameters->constellation_count *
			   V92_CPU_CONSTELLATION_BITS +
		   V92_CPU_MIN_TRAILER_BITS;
	multiple = (size_t)bits_per_symbol * V92_CPU_SYMBOLS_PER_FRAME;
	return (unfilled + multiple - 1u) / multiple * multiple;
}

size_t v92_cpu_frame_bytes(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol)
{
	size_t bits = v92_cpu_frame_bits(parameters, bits_per_symbol);

	return bits == 0u ? 0u : (bits + 7u) / 8u;
}

static size_t map_first_bit(size_t interval)
{
	return CPU_MAP_FIRST_BIT + interval * CPU_MAP_BITS +
	       (interval >= 4u ? 1u : 0u);
}

static size_t mask_bit(
	size_t row, uint8_t ucode)
{
	return CPU_MASKS_FIRST_BIT +
	       row * V92_CPU_CONSTELLATION_BITS +
	       (size_t)(ucode / 16u) * 17u + 1u + ucode % 16u;
}

static void encode_header(
	uint8_t *frame, const struct v92_cpu_parameters *parameters)
{
	for (size_t bit = 0u; bit < 17u; ++bit)
		packed_msb_set_bit(frame, bit, true);
	packed_msb_write_lsb_field(
		frame, CPU_TYPE_FIRST_BIT, CPU_TYPE_BITS, 1u);
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

static void encode_constellation_set(
	uint8_t *frame, size_t first_row,
	const struct pcm_constellation *constellations,
	size_t count)
{
	for (size_t index = 0u; index < count; ++index) {
		for (uint8_t ucode = 0u;
		     ucode < PCM_CONSTELLATION_UCODES; ++ucode) {
			packed_msb_set_bit(
				frame, mask_bit(first_row + index, ucode),
				pcm_constellation_contains(
					&constellations[index], ucode));
		}
	}
}

static uint16_t frame_crc(
	const uint8_t *frame, size_t payload_end)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t bit = CPU_INFORMATION_FIRST_BIT;
	     bit < payload_end; ++bit) {
		if (bit % 17u == 0u)
			continue;
		remainder = crc16_ccitt_false_step(
			remainder, packed_msb_bit(frame, bit) != 0u);
	}
	return remainder;
}

enum v92_cpu_result v92_cpu_encode(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol,
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
	frame_bits = v92_cpu_frame_bits(parameters, bits_per_symbol);
	if (frame_bits == 0u)
		return V92_CPU_INVALID_PARAMETERS;
	frame_bytes = (frame_bits + 7u) / 8u;
	if (frame_capacity < frame_bytes)
		return V92_CPU_OUTPUT_TOO_SMALL;

	encode_header(encoded, parameters);
	encode_constellation_set(
		encoded, 0u, parameters->transmit_constellations,
		parameters->constellation_count);
	sets = parameters->codec_constellations_differ ? 2u : 1u;
	if (sets == 2u) {
		encode_constellation_set(
			encoded, parameters->constellation_count,
			parameters->codec_constellations,
			parameters->constellation_count);
	}
	payload_end = CPU_MASKS_FIRST_BIT + sets *
		parameters->constellation_count *
		V92_CPU_CONSTELLATION_BITS;
	crc = frame_crc(encoded, payload_end);
	packed_msb_write_msb_field(
		encoded, payload_end + 1u, CPU_CRC_BITS, crc);
	memcpy(frame, encoded, frame_bytes);
	*frame_bit_count = frame_bits;
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
	}
	return "unknown";
}
