// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_info1a.h"

#include "packed_bits.h"

#include <string.h>

enum {
	FILTER_SECTIONS_FIRST_BIT = 0,
	FILTER_SECTIONS_BITS = 2,
	TOTAL_COEFFICIENTS_FIRST_BIT = 2,
	TOTAL_COEFFICIENTS_BITS = 2,
	SECTION_COEFFICIENTS_FIRST_BIT = 4,
	SECTION_COEFFICIENTS_BITS = 2,
	MD_LENGTH_FIRST_BIT = 6,
	MD_LENGTH_BITS = 7,
	TRAINING_UCODE_FIRST_BIT = 13,
	TRAINING_UCODE_BITS = 7,
	ANALOG_SYMBOL_RATE_FIRST_BIT = 22,
	DIGITAL_SYMBOL_RATE_FIRST_BIT = 25,
	SYMBOL_RATE_BITS = 3,
	PCM_SYMBOL_RATE_CODE = 6,
	RESERVED_ONES_FIRST_BIT = 28,
	RESERVED_ONES_BITS = 10,
};

static bool filter_sections_are_valid(
	enum v92_pcm_filter_sections sections)
{
	return sections >= V92_PCM_FILTER_P1_Z2 &&
	       sections <= V92_PCM_FILTER_Z1_P1_P2_Z2;
}

static bool coefficient_count_is_valid(
	uint16_t count, uint16_t minimum, uint16_t maximum)
{
	return count >= minimum && count <= maximum &&
	       (count - minimum) % V92_PCM_INFO1A_COEFFICIENT_STEP == 0;
}

bool v92_pcm_info1a_is_valid(const struct v92_pcm_info1a *info1a)
{
	return info1a != NULL &&
	       filter_sections_are_valid(info1a->filter_sections) &&
	       coefficient_count_is_valid(
		       info1a->maximum_total_coefficients,
		       V92_PCM_INFO1A_MIN_TOTAL_COEFFICIENTS,
		       V92_PCM_INFO1A_MAX_TOTAL_COEFFICIENTS) &&
	       coefficient_count_is_valid(
		       info1a->maximum_section_coefficients,
		       V92_PCM_INFO1A_MIN_SECTION_COEFFICIENTS,
		       V92_PCM_INFO1A_MAX_SECTION_COEFFICIENTS) &&
	       info1a->maximum_total_coefficients >=
		       info1a->maximum_section_coefficients &&
	       info1a->local_md_length_276_symbol_units <=
		       V92_PCM_INFO1A_MAX_MD_LENGTH_UNITS &&
	       info1a->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       info1a->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE;
}

bool v92_pcm_info1a_equal(
	const struct v92_pcm_info1a *left,
	const struct v92_pcm_info1a *right)
{
	return v92_pcm_info1a_is_valid(left) &&
	       v92_pcm_info1a_is_valid(right) &&
	       left->filter_sections == right->filter_sections &&
	       left->maximum_total_coefficients ==
		       right->maximum_total_coefficients &&
	       left->maximum_section_coefficients ==
		       right->maximum_section_coefficients &&
	       left->local_md_length_276_symbol_units ==
		       right->local_md_length_276_symbol_units &&
	       left->training_ucode == right->training_ucode;
}

static uint16_t coefficient_code(uint16_t count, uint16_t minimum)
{
	return (uint16_t)(
		(count - minimum) / V92_PCM_INFO1A_COEFFICIENT_STEP);
}

enum v92_pcm_info1a_result v92_pcm_info1a_encode(
	const struct v92_pcm_info1a *info1a, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V92_PCM_INFO1A_PAYLOAD_BYTES] = { 0 };

	if (info1a == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V92_PCM_INFO1A_INVALID_ARGUMENT;
	if (!v92_pcm_info1a_is_valid(info1a))
		return V92_PCM_INFO1A_INVALID_SELECTION;
	packed_msb_write_lsb_field(
		encoded, FILTER_SECTIONS_FIRST_BIT, FILTER_SECTIONS_BITS,
		(uint16_t)info1a->filter_sections);
	packed_msb_write_lsb_field(
		encoded, TOTAL_COEFFICIENTS_FIRST_BIT,
		TOTAL_COEFFICIENTS_BITS,
		coefficient_code(
			info1a->maximum_total_coefficients,
			V92_PCM_INFO1A_MIN_TOTAL_COEFFICIENTS));
	packed_msb_write_lsb_field(
		encoded, SECTION_COEFFICIENTS_FIRST_BIT,
		SECTION_COEFFICIENTS_BITS,
		coefficient_code(
			info1a->maximum_section_coefficients,
			V92_PCM_INFO1A_MIN_SECTION_COEFFICIENTS));
	packed_msb_write_lsb_field(
		encoded, MD_LENGTH_FIRST_BIT, MD_LENGTH_BITS,
		info1a->local_md_length_276_symbol_units);
	packed_msb_write_lsb_field(
		encoded, TRAINING_UCODE_FIRST_BIT, TRAINING_UCODE_BITS,
		info1a->training_ucode);
	packed_msb_write_lsb_field(
		encoded, ANALOG_SYMBOL_RATE_FIRST_BIT, SYMBOL_RATE_BITS,
		PCM_SYMBOL_RATE_CODE);
	packed_msb_write_lsb_field(
		encoded, DIGITAL_SYMBOL_RATE_FIRST_BIT, SYMBOL_RATE_BITS,
		PCM_SYMBOL_RATE_CODE);
	for (size_t bit = RESERVED_ONES_FIRST_BIT;
	     bit < RESERVED_ONES_FIRST_BIT + RESERVED_ONES_BITS; ++bit)
		packed_msb_set_bit(encoded, bit, true);
	memcpy(payload, encoded, sizeof(encoded));
	return V92_PCM_INFO1A_OK;
}

enum v92_pcm_info1a_result v92_pcm_info1a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_pcm_info1a *info1a)
{
	struct v92_pcm_info1a decoded;

	if (payload == NULL || info1a == NULL ||
	    payload_capacity < V92_PCM_INFO1A_PAYLOAD_BYTES)
		return V92_PCM_INFO1A_INVALID_ARGUMENT;
	if (payload_bit_count != V92_PCM_INFO1A_PAYLOAD_BITS ||
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count,
		    V92_PCM_INFO1A_PAYLOAD_BYTES) ||
	    packed_msb_read_lsb_field(
		    payload, ANALOG_SYMBOL_RATE_FIRST_BIT,
		    SYMBOL_RATE_BITS) != PCM_SYMBOL_RATE_CODE ||
	    packed_msb_read_lsb_field(
		    payload, DIGITAL_SYMBOL_RATE_FIRST_BIT,
		    SYMBOL_RATE_BITS) != PCM_SYMBOL_RATE_CODE)
		return V92_PCM_INFO1A_INVALID_PAYLOAD;
	decoded = (struct v92_pcm_info1a){
		.filter_sections =
			(enum v92_pcm_filter_sections)packed_msb_read_lsb_field(
				payload, FILTER_SECTIONS_FIRST_BIT,
				FILTER_SECTIONS_BITS),
		.maximum_total_coefficients = (uint16_t)(
			V92_PCM_INFO1A_MIN_TOTAL_COEFFICIENTS +
			V92_PCM_INFO1A_COEFFICIENT_STEP *
				packed_msb_read_lsb_field(
					payload, TOTAL_COEFFICIENTS_FIRST_BIT,
					TOTAL_COEFFICIENTS_BITS)),
		.maximum_section_coefficients = (uint16_t)(
			V92_PCM_INFO1A_MIN_SECTION_COEFFICIENTS +
			V92_PCM_INFO1A_COEFFICIENT_STEP *
				packed_msb_read_lsb_field(
					payload, SECTION_COEFFICIENTS_FIRST_BIT,
					SECTION_COEFFICIENTS_BITS)),
		.local_md_length_276_symbol_units =
			(uint8_t)packed_msb_read_lsb_field(
				payload, MD_LENGTH_FIRST_BIT,
				MD_LENGTH_BITS),
		.training_ucode = (uint8_t)packed_msb_read_lsb_field(
			payload, TRAINING_UCODE_FIRST_BIT,
			TRAINING_UCODE_BITS),
	};
	if (!v92_pcm_info1a_is_valid(&decoded))
		return V92_PCM_INFO1A_INVALID_PAYLOAD;
	*info1a = decoded;
	return V92_PCM_INFO1A_OK;
}

const char *v92_pcm_info1a_result_name(
	enum v92_pcm_info1a_result result)
{
	switch (result) {
	case V92_PCM_INFO1A_OK:
		return "ok";
	case V92_PCM_INFO1A_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_INFO1A_INVALID_SELECTION:
		return "invalid selection";
	case V92_PCM_INFO1A_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
