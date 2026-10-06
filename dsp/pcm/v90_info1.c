// SPDX-License-Identifier: BSD-3-Clause

#include "v90_info1.h"

#include "integer_math.h"
#include "packed_bits.h"

#include <string.h>

enum {
	MD_LENGTH_FIRST_BIT = 6,
	MD_LENGTH_BITS = 7,
	TRAINING_UCODE_FIRST_BIT = 13,
	TRAINING_UCODE_BITS = 7,
	UPSTREAM_SYMBOL_RATE_FIRST_BIT = 22,
	SYMBOL_RATE_BITS = 3,
	DOWNSTREAM_SYMBOL_RATE_FIRST_BIT = 25,
	DOWNSTREAM_SYMBOL_RATE_CODE = 6,
	FREQUENCY_OFFSET_FIRST_BIT = 28,
	FREQUENCY_OFFSET_BITS = 10,
};

bool v90_info1a_is_valid(const struct v90_info1a *info1a)
{
	return info1a != NULL &&
	       info1a->local_md_length_35_ms_units <=
		       V34_INFO1_MAX_MD_LENGTH_35_MS_UNITS &&
	       info1a->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       info1a->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       info1a->upstream_symbol_rate >= V34_SYMBOL_RATE_3000 &&
	       info1a->upstream_symbol_rate <= V34_SYMBOL_RATE_3429 &&
	       info1a->frequency_offset_1_50_hz_units >=
		       V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE &&
	       info1a->frequency_offset_1_50_hz_units <=
		       V34_INFO1_MAX_FREQUENCY_OFFSET_1_50_HZ_UNITS;
}

enum v90_info1_result v90_info1a_encode(
	const struct v90_info1a *info1a, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V90_INFO1A_PAYLOAD_BYTES] = { 0 };

	if (info1a == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V90_INFO1_INVALID_ARGUMENT;
	if (!v90_info1a_is_valid(info1a))
		return V90_INFO1_INVALID_SELECTION;
	packed_msb_write_lsb_field(
		encoded, MD_LENGTH_FIRST_BIT, MD_LENGTH_BITS,
		info1a->local_md_length_35_ms_units);
	packed_msb_write_lsb_field(
		encoded, TRAINING_UCODE_FIRST_BIT, TRAINING_UCODE_BITS,
		info1a->training_ucode);
	packed_msb_write_lsb_field(
		encoded, UPSTREAM_SYMBOL_RATE_FIRST_BIT, SYMBOL_RATE_BITS,
		(uint16_t)info1a->upstream_symbol_rate);
	packed_msb_write_lsb_field(
		encoded, DOWNSTREAM_SYMBOL_RATE_FIRST_BIT, SYMBOL_RATE_BITS,
		DOWNSTREAM_SYMBOL_RATE_CODE);
	packed_msb_write_lsb_field(
		encoded, FREQUENCY_OFFSET_FIRST_BIT, FREQUENCY_OFFSET_BITS,
		(uint16_t)info1a->frequency_offset_1_50_hz_units);
	memcpy(payload, encoded, sizeof(encoded));
	return V90_INFO1_OK;
}

enum v90_info1_result v90_info1a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info1a *info1a)
{
	struct v90_info1a decoded;

	if (payload == NULL || info1a == NULL ||
	    payload_capacity < V90_INFO1A_PAYLOAD_BYTES)
		return V90_INFO1_INVALID_ARGUMENT;
	if (payload_bit_count != V90_INFO1A_PAYLOAD_BITS ||
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count, V90_INFO1A_PAYLOAD_BYTES) ||
	    packed_msb_read_lsb_field(
		    payload, DOWNSTREAM_SYMBOL_RATE_FIRST_BIT,
		    SYMBOL_RATE_BITS) != DOWNSTREAM_SYMBOL_RATE_CODE)
		return V90_INFO1_INVALID_PAYLOAD;
	decoded = (struct v90_info1a){
		.local_md_length_35_ms_units =
			(uint8_t)packed_msb_read_lsb_field(
				payload, MD_LENGTH_FIRST_BIT, MD_LENGTH_BITS),
		.training_ucode = (uint8_t)packed_msb_read_lsb_field(
			payload, TRAINING_UCODE_FIRST_BIT,
			TRAINING_UCODE_BITS),
		.upstream_symbol_rate =
			(enum v34_symbol_rate)packed_msb_read_lsb_field(
				payload, UPSTREAM_SYMBOL_RATE_FIRST_BIT,
				SYMBOL_RATE_BITS),
		.frequency_offset_1_50_hz_units =
			(int16_t)arithmetic_shift_right_u32(
				packed_msb_read_lsb_field(
					payload, FREQUENCY_OFFSET_FIRST_BIT, FREQUENCY_OFFSET_BITS) <<
					(32u - FREQUENCY_OFFSET_BITS),
				32u - FREQUENCY_OFFSET_BITS),
	};
	if (!v90_info1a_is_valid(&decoded))
		return V90_INFO1_INVALID_PAYLOAD;
	*info1a = decoded;
	return V90_INFO1_OK;
}

const char *v90_info1_result_name(enum v90_info1_result result)
{
	switch (result) {
	case V90_INFO1_OK:
		return "ok";
	case V90_INFO1_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_INFO1_INVALID_SELECTION:
		return "invalid selection";
	case V90_INFO1_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
