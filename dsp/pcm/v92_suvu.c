// SPDX-License-Identifier: BSD-3-Clause

#include "v92_suvu.h"

#include "crc16_ccitt_false.h"
#include "packed_bits.h"

#include <string.h>

enum {
	SUVU_INFORMATION_FIRST_BIT = 18,
	SUVU_KIND_BIT = 18,
	SUVU_WAIT_FOR_CPU_BIT = 26,
	SUVU_LEVEL_FIRST_BIT = 27,
	SUVU_LEVEL_BITS = 5,
	SUVU_REQUEST_SILENCE_BIT = 32,
	SUVU_ACKNOWLEDGMENT_BIT = 33,
	SUVU_CRC_START_BIT = 34,
	SUVU_CRC_FIRST_BIT = 35,
	SUVU_CRC_BITS = 16,
	SUVU_FILL_FIRST_BIT = 51,
};

static unsigned int bits_per_symbol(
	enum v92_constellation_size constellation)
{
	switch (constellation) {
	case V92_CONSTELLATION_4_POINT:
		return 2;
	case V92_CONSTELLATION_8_POINT:
		return 3;
	}
	return 0;
}

size_t v92_suvu_frame_bits(enum v92_constellation_size constellation)
{
	unsigned int bits = bits_per_symbol(constellation);
	size_t multiple;
	size_t unfilled;

	if (bits == 0)
		return 0;
	multiple = (size_t)bits * V92_SUVU_SYMBOLS_PER_FRAME;
	unfilled = V92_SUVU_INFORMATION_BITS + V92_SUVU_TRAILER_BITS;
	return (unfilled + multiple - 1u) / multiple * multiple;
}

size_t v92_suvu_frame_bytes(enum v92_constellation_size constellation)
{
	size_t bits = v92_suvu_frame_bits(constellation);

	return bits == 0 ? 0 : (bits + 7u) / 8u;
}

bool v92_suvu_parameters_are_valid(
	const struct v92_suvu_parameters *parameters)
{
	return parameters != NULL &&
	       parameters->prefilter_level_db_q2_2 >= -16 &&
	       parameters->prefilter_level_db_q2_2 <= 15;
}

static void encode_lsb_first(
	uint8_t *frame, size_t first_bit, size_t bit_count, uint32_t value)
{
	for (size_t index = 0; index < bit_count; ++index) {
		packed_msb_set_bit(
			frame, first_bit + index,
			(value & (UINT32_C(1) << index)) != 0);
	}
}

static uint32_t decode_lsb_first(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	uint32_t value = 0;

	for (size_t index = 0; index < bit_count; ++index) {
		value |= (uint32_t)packed_msb_bit(frame, first_bit + index) <<
			 index;
	}
	return value;
}

static uint16_t frame_crc(const uint8_t *frame)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t bit = SUVU_INFORMATION_FIRST_BIT;
	     bit < SUVU_CRC_START_BIT; ++bit) {
		remainder = crc16_ccitt_false_step(
			remainder, packed_msb_bit(frame, bit) != 0);
	}
	return remainder;
}

static bool bits_are(
	const uint8_t *frame, size_t first_bit, size_t bit_count, uint8_t value)
{
	for (size_t index = 0; index < bit_count; ++index) {
		if (packed_msb_bit(frame, first_bit + index) != value)
			return false;
	}
	return true;
}

enum v92_suvu_result v92_suvu_encode(
	const struct v92_suvu_parameters *parameters,
	enum v92_constellation_size constellation,
	uint8_t *frame, size_t frame_capacity, size_t *frame_bit_count)
{
	uint8_t encoded[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t bit_count = v92_suvu_frame_bits(constellation);
	uint8_t raw_level;
	uint16_t crc;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_SUVU_INVALID_ARGUMENT;
	if (!v92_suvu_parameters_are_valid(parameters) ||
	    bit_count != V92_SUVU_FRAME_BITS)
		return V92_SUVU_INVALID_PARAMETERS;
	if (frame_capacity < sizeof(encoded))
		return V92_SUVU_OUTPUT_TOO_SMALL;

	for (size_t bit = 0; bit < 17u; ++bit)
		packed_msb_set_bit(encoded, bit, true);
	packed_msb_set_bit(encoded, SUVU_KIND_BIT, true);
	packed_msb_set_bit(
		encoded, SUVU_WAIT_FOR_CPU_BIT, parameters->wait_for_cpu);
	raw_level = parameters->prefilter_level_db_q2_2 < 0 ?
		(uint8_t)(parameters->prefilter_level_db_q2_2 + 32) :
		(uint8_t)parameters->prefilter_level_db_q2_2;
	encode_lsb_first(
		encoded, SUVU_LEVEL_FIRST_BIT, SUVU_LEVEL_BITS, raw_level);
	packed_msb_set_bit(
		encoded, SUVU_REQUEST_SILENCE_BIT,
		parameters->request_silence);
	packed_msb_set_bit(
		encoded, SUVU_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_cpd);
	crc = frame_crc(encoded);
	for (size_t bit = 0; bit < SUVU_CRC_BITS; ++bit) {
		packed_msb_set_bit(
			encoded, SUVU_CRC_FIRST_BIT + bit,
			(crc & (uint16_t)(1u <<
				(SUVU_CRC_BITS - bit - 1u))) != 0);
	}
	memcpy(frame, encoded, sizeof(encoded));
	*frame_bit_count = bit_count;
	return V92_SUVU_OK;
}

static bool framing_is_valid(
	const uint8_t *frame, size_t frame_bit_count)
{
	return bits_are(frame, 0, 17, 1) &&
	       packed_msb_bit(frame, 17) == 0 &&
	       packed_msb_bit(frame, SUVU_KIND_BIT) == 1 &&
	       packed_msb_bit(frame, SUVU_CRC_START_BIT) == 0 &&
	       bits_are(
		       frame, SUVU_FILL_FIRST_BIT,
		       frame_bit_count - SUVU_FILL_FIRST_BIT, 0);
}

static uint16_t decode_crc(const uint8_t *frame)
{
	uint16_t crc = 0;

	for (size_t bit = 0; bit < SUVU_CRC_BITS; ++bit) {
		crc = (uint16_t)((crc << 1) |
			packed_msb_bit(frame, SUVU_CRC_FIRST_BIT + bit));
	}
	return crc;
}

enum v92_suvu_result v92_suvu_decode(
	const uint8_t *frame, size_t frame_capacity, size_t frame_bit_count,
	enum v92_constellation_size constellation,
	struct v92_suvu_parameters *parameters)
{
	struct v92_suvu_parameters decoded;
	uint8_t raw_level;
	size_t expected_bits = v92_suvu_frame_bits(constellation);
	size_t expected_bytes = v92_suvu_frame_bytes(constellation);

	if (frame == NULL || parameters == NULL)
		return V92_SUVU_INVALID_ARGUMENT;
	if (expected_bits != V92_SUVU_FRAME_BITS ||
	    frame_bit_count != expected_bits)
		return V92_SUVU_INVALID_FRAME;
	if (frame_capacity < expected_bytes)
		return V92_SUVU_INVALID_ARGUMENT;
	if (!framing_is_valid(frame, frame_bit_count))
		return V92_SUVU_INVALID_FRAME;
	if (decode_crc(frame) != frame_crc(frame))
		return V92_SUVU_CRC_MISMATCH;

	raw_level = (uint8_t)decode_lsb_first(
		frame, SUVU_LEVEL_FIRST_BIT, SUVU_LEVEL_BITS);
	decoded = (struct v92_suvu_parameters){
		.prefilter_level_db_q2_2 = raw_level < 16u ?
			(int8_t)raw_level : (int8_t)((int)raw_level - 32),
		.wait_for_cpu =
			packed_msb_bit(frame, SUVU_WAIT_FOR_CPU_BIT) != 0,
		.request_silence =
			packed_msb_bit(frame, SUVU_REQUEST_SILENCE_BIT) != 0,
		.acknowledges_cpd =
			packed_msb_bit(frame, SUVU_ACKNOWLEDGMENT_BIT) != 0,
	};
	if (!v92_suvu_parameters_are_valid(&decoded))
		return V92_SUVU_INVALID_FRAME;
	*parameters = decoded;
	return V92_SUVU_OK;
}

const char *v92_suvu_result_name(enum v92_suvu_result result)
{
	switch (result) {
	case V92_SUVU_OK:
		return "ok";
	case V92_SUVU_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_SUVU_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_SUVU_OUTPUT_TOO_SMALL:
		return "output too small";
	case V92_SUVU_INVALID_FRAME:
		return "invalid frame";
	case V92_SUVU_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
