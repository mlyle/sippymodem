// SPDX-License-Identifier: BSD-3-Clause

#include "v90_dil.h"

#include "memory_region.h"
#include "packed_bits.h"
#include "pcm_dil_descriptor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	N_FIRST_BIT = 18,
	N_BITS = 8,
	LSP_MINUS_ONE_FIRST_BIT = 35,
	LTP_MINUS_ONE_FIRST_BIT = 43,
	LENGTH_BITS = 7,
	PATTERN_DATA_FIRST_BIT = 52,
	U_CODES_START_OFFSET = 187,
	CRC_AND_FILL_BITS = 18,
};

static void set_pattern_word(
	uint8_t *pattern, size_t first_bit, uint64_t word,
	size_t available_bits)
{
	for (size_t bit = 0; bit < available_bits; ++bit) {
		if (((word >> bit) & UINT64_C(1)) != 0u)
			pattern[(first_bit + bit) / 8u] |=
				(uint8_t)(1u << ((first_bit + bit) % 8u));
	}
}

static void conventional_training_ucodes(uint8_t *ucodes)
{
	for (size_t index = 0; index < 16u; ++index)
		ucodes[index] = (uint8_t)(0x4fu - index);
	for (size_t index = 16u; index < 47u; ++index)
		ucodes[index] = (uint8_t)(0x3fu - 2u * (index - 16u));
	ucodes[47] = 0x02u;
	for (size_t index = 48u; index < 79u; ++index)
		ucodes[index] = (uint8_t)(0x04u + 2u * (index - 48u));
	for (size_t index = 79u; index < 131u; ++index)
		ucodes[index] = (uint8_t)(0x41u + index - 79u);
	ucodes[131] = 0x64u;
	ucodes[132] = 0x5au;
	ucodes[133] = 0x50u;
	for (size_t index = 134u; index < 144u; ++index)
		ucodes[index] = (uint8_t)(0x4fu - (index - 134u));
}

bool v90_dil_profile_is_valid(enum v90_dil_profile profile)
{
	return profile == V90_DIL_PROFILE_NONE ||
	       profile == V90_DIL_PROFILE_CONVENTIONAL;
}

enum v90_dil_result v90_dil_profile_init(
	struct v90_dil_parameters *parameters,
	enum v90_dil_profile profile)
{
	static const uint8_t segment_length_codes[V90_DIL_CHORDS] = {
		0x13, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x13,
	};
	static const uint8_t reference_ucodes[V90_DIL_CHORDS] = {
		0x4e, 0x4e, 0x4e, 0x4e, 0x4e, 0x4e, 0x4e, 0x19,
	};
	struct v90_dil_parameters generated = {
		.sign_pattern_bits = 1,
		.training_pattern_bits = 1,
	};

	if (parameters == NULL || !v90_dil_profile_is_valid(profile))
		return V90_DIL_INVALID_ARGUMENT;
	if (profile == V90_DIL_PROFILE_CONVENTIONAL) {
		/* This fixed profile spans the useful companding levels and a full
		 * 120-bit sign/training cycle. Pack the reviewed patterns into words
		 * so their generator remains smaller than the wire image. */
		generated.segment_count = 144u;
		generated.sign_pattern_bits = 120u;
		generated.training_pattern_bits = 120u;
		set_pattern_word(
			generated.sign_pattern, 0,
			UINT64_C(0x7766519abe3405f1), 64u);
		set_pattern_word(
			generated.sign_pattern, 64u,
			UINT64_C(0x009d82b53bbc098a), 56u);
		set_pattern_word(
			generated.training_pattern, 0,
			UINT64_C(0x3dbe054a81fc0ad6), 64u);
		set_pattern_word(
			generated.training_pattern, 64u,
			UINT64_C(0x00f4ad599ac64ad4), 56u);
		memcpy(
			generated.segment_length_codes, segment_length_codes,
			sizeof(segment_length_codes));
		memcpy(
			generated.reference_ucodes, reference_ucodes,
			sizeof(reference_ucodes));
		conventional_training_ucodes(generated.training_ucodes);
	}
	if (!v90_dil_parameters_are_valid(&generated))
		return V90_DIL_INVALID_FRAME;
	*parameters = generated;
	return V90_DIL_OK;
}

bool v90_dil_training_symbol_count(
	const struct v90_dil_parameters *parameters, uint32_t *symbols)
{
	uint32_t total = 0;

	if (!v90_dil_parameters_are_valid(parameters) || symbols == NULL)
		return false;
	for (size_t segment = 0; segment < parameters->segment_count;
	     ++segment) {
		uint8_t chord = parameters->training_ucodes[segment] >> 4;

		total += (uint32_t)(parameters->segment_length_codes[chord] + 1u) *
			 V90_DIL_SEGMENT_INTERVALS;
	}
	*symbols = total;
	return true;
}

static bool pattern_is_valid(
	const uint8_t *pattern, uint8_t pattern_bits)
{
	size_t used_bytes;
	uint8_t final_mask;

	if (pattern_bits == 0u || pattern_bits > V90_DIL_MAX_PATTERN_BITS)
		return false;
	used_bytes = (pattern_bits + 7u) / 8u;
	if (pattern_bits % 8u != 0u) {
		final_mask = (uint8_t)~((1u << (pattern_bits % 8u)) - 1u);
		if ((pattern[used_bytes - 1u] & final_mask) != 0u)
			return false;
	}
	return memory_region_is_zero(
		pattern + used_bytes, V90_DIL_PATTERN_BYTES - used_bytes);
}

bool v90_dil_parameters_are_valid(
	const struct v90_dil_parameters *parameters)
{
	if (parameters == NULL ||
	    !pattern_is_valid(
		    parameters->sign_pattern,
		    parameters->sign_pattern_bits) ||
	    !pattern_is_valid(
		    parameters->training_pattern,
		    parameters->training_pattern_bits) ||
	    (parameters->segment_count == 0u &&
	     (parameters->sign_pattern_bits != 1u ||
	      parameters->training_pattern_bits != 1u)))
		return false;
	for (size_t chord = 0; chord < V90_DIL_CHORDS; ++chord) {
		if (parameters->segment_length_codes[chord] > 127u ||
		    parameters->reference_ucodes[chord] > 127u)
			return false;
	}
	for (size_t segment = 0; segment < parameters->segment_count;
	     ++segment) {
		if (parameters->training_ucodes[segment] > 127u)
			return false;
	}
	return memory_region_is_zero(
		parameters->training_ucodes + parameters->segment_count,
		V90_DIL_MAX_SEGMENTS - parameters->segment_count);
}

static bool initial_framing_is_valid(const uint8_t *descriptor)
{
	for (size_t bit = 0; bit < V90_DIL_FRAME_SYNC_BITS; ++bit) {
		if (packed_msb_bit(descriptor, bit) == 0u)
			return false;
	}
	return packed_msb_bit(descriptor, 17u) == 0u &&
	       packed_msb_bit(descriptor, 34u) == 0u;
}

static bool layout_from_lengths(
	uint8_t segment_count, uint8_t sign_pattern_bits,
	uint8_t training_pattern_bits,
	struct pcm_dil_descriptor_layout *layout)
{
	size_t sign_span;
	size_t training_span;
	size_t segment_span;
	struct pcm_dil_descriptor_layout generated;

	if (layout == NULL || sign_pattern_bits == 0u ||
	    sign_pattern_bits > V90_DIL_MAX_PATTERN_BITS ||
	    training_pattern_bits == 0u ||
	    training_pattern_bits > V90_DIL_MAX_PATTERN_BITS)
		return false;
	sign_span = ((sign_pattern_bits + 15u) / 16u) * 17u;
	training_span = ((training_pattern_bits + 15u) / 16u) * 17u;
	segment_span = ((segment_count + 1u) / 2u) * 17u;
	generated.crc_start_bit = U_CODES_START_OFFSET + sign_span +
		training_span + segment_span;
	generated.bit_count = generated.crc_start_bit + CRC_AND_FILL_BITS;
	if (generated.bit_count % 2u != 0u)
		generated.bit_count++;
	if (generated.bit_count > V90_DIL_MAX_DESCRIPTOR_BITS ||
	    !pcm_dil_descriptor_layout_is_valid(&generated))
		return false;
	*layout = generated;
	return true;
}

enum v90_dil_result v90_dil_descriptor_layout(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t received_bits, struct pcm_dil_descriptor_layout *layout)
{
	struct pcm_dil_descriptor_layout generated;
	uint32_t segment_count;
	uint32_t sign_pattern_minus_one;
	uint32_t training_pattern_minus_one;

	if (descriptor == NULL || layout == NULL ||
	    received_bits > V90_DIL_MAX_DESCRIPTOR_BITS ||
	    descriptor_capacity < (received_bits + 7u) / 8u)
		return V90_DIL_INVALID_ARGUMENT;
	if (received_bits < V90_DIL_HEADER_BITS)
		return V90_DIL_INCOMPLETE;
	if (!initial_framing_is_valid(descriptor))
		return V90_DIL_INVALID_FRAME;
	segment_count = packed_msb_read_lsb_field(
		descriptor, N_FIRST_BIT, N_BITS);
	sign_pattern_minus_one = packed_msb_read_lsb_field(
		descriptor, LSP_MINUS_ONE_FIRST_BIT, LENGTH_BITS);
	training_pattern_minus_one = packed_msb_read_lsb_field(
		descriptor, LTP_MINUS_ONE_FIRST_BIT, LENGTH_BITS);
	if (!layout_from_lengths(
		    (uint8_t)segment_count,
		    (uint8_t)(sign_pattern_minus_one + 1u),
		    (uint8_t)(training_pattern_minus_one + 1u), &generated))
		return V90_DIL_INVALID_FRAME;
	if (received_bits > generated.bit_count)
		return V90_DIL_INVALID_FRAME;
	*layout = generated;
	return V90_DIL_OK;
}

static size_t pattern_wire_bit(size_t first_bit, size_t pattern_bit)
{
	return first_bit + pattern_bit + pattern_bit / 16u;
}

static bool pattern_bit(const uint8_t *pattern, size_t bit)
{
	return ((pattern[bit / 8u] >> (bit % 8u)) & 1u) != 0u;
}

static void set_pattern_bit(uint8_t *pattern, size_t bit, bool value)
{
	uint8_t mask = (uint8_t)(1u << (bit % 8u));

	if (value)
		pattern[bit / 8u] |= mask;
}

static size_t sign_pattern_span(uint8_t sign_pattern_bits)
{
	return ((sign_pattern_bits + 15u) / 16u) * 17u;
}

static size_t fixed_pair_first_bit(size_t patterns_span, size_t pair)
{
	return PATTERN_DATA_FIRST_BIT + patterns_span + pair * 17u;
}

static bool write_field(
	uint8_t *descriptor,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits, uint32_t value)
{
	return pcm_dil_descriptor_write_lsb_field(
		descriptor, V90_DIL_MAX_DESCRIPTOR_BYTES, layout,
		first_bit, field_bits, value);
}

static bool read_field(
	const uint8_t *descriptor, size_t descriptor_capacity,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits, uint32_t *value)
{
	return pcm_dil_descriptor_read_lsb_field(
		descriptor, descriptor_capacity, layout,
		first_bit, field_bits, value);
}

enum v90_dil_result v90_dil_payload_encode(
	const struct v90_dil_parameters *parameters,
	uint8_t *descriptor, size_t descriptor_capacity,
	struct pcm_dil_descriptor_layout *payload_layout)
{
	uint8_t encoded[V90_DIL_MAX_DESCRIPTOR_BYTES];
	struct pcm_dil_descriptor_layout layout;
	size_t sign_span;
	size_t patterns_span;
	size_t bytes;

	if (!v90_dil_parameters_are_valid(parameters) || descriptor == NULL ||
	    payload_layout == NULL ||
	    !layout_from_lengths(
		    parameters->segment_count,
		    parameters->sign_pattern_bits,
		    parameters->training_pattern_bits, &layout))
		return V90_DIL_INVALID_ARGUMENT;
	bytes = pcm_dil_descriptor_storage_bytes(&layout);
	if (descriptor_capacity < bytes)
		return V90_DIL_INVALID_ARGUMENT;
	memset(encoded, 0, sizeof(encoded));
	if (pcm_dil_descriptor_initialize(
		    encoded, sizeof(encoded), &layout) !=
		    PCM_DIL_DESCRIPTOR_OK ||
	    !write_field(
		    encoded, &layout, N_FIRST_BIT, N_BITS,
		    parameters->segment_count) ||
	    !write_field(
		    encoded, &layout, LSP_MINUS_ONE_FIRST_BIT, LENGTH_BITS,
		    parameters->sign_pattern_bits - 1u) ||
	    !write_field(
		    encoded, &layout, LTP_MINUS_ONE_FIRST_BIT, LENGTH_BITS,
		    parameters->training_pattern_bits - 1u))
		return V90_DIL_INVALID_FRAME;
	for (size_t bit = 0; bit < parameters->sign_pattern_bits; ++bit)
		packed_msb_set_bit(
			encoded,
			pattern_wire_bit(PATTERN_DATA_FIRST_BIT, bit),
			pattern_bit(parameters->sign_pattern, bit));
	sign_span = sign_pattern_span(parameters->sign_pattern_bits);
	for (size_t bit = 0; bit < parameters->training_pattern_bits; ++bit)
		packed_msb_set_bit(
			encoded,
			pattern_wire_bit(
				PATTERN_DATA_FIRST_BIT + sign_span, bit),
			pattern_bit(parameters->training_pattern, bit));
	patterns_span = sign_span +
		sign_pattern_span(parameters->training_pattern_bits);
	for (size_t chord = 0; chord < V90_DIL_CHORDS; ++chord) {
		size_t h_bit = fixed_pair_first_bit(
			patterns_span, chord / 2u) + (chord % 2u) * 8u;
		size_t reference_bit = fixed_pair_first_bit(
			patterns_span, 4u + chord / 2u) +
			(chord % 2u) * 8u;

		if (!write_field(
			    encoded, &layout, h_bit, 7u,
			    parameters->segment_length_codes[chord]) ||
		    !write_field(
			    encoded, &layout, reference_bit, 7u,
			    parameters->reference_ucodes[chord]))
			return V90_DIL_INVALID_FRAME;
	}
	for (size_t segment = 0; segment < parameters->segment_count;
	     ++segment) {
		size_t ucode_bit = fixed_pair_first_bit(
			patterns_span, 8u + segment / 2u) +
			(segment % 2u) * 8u;

		if (!write_field(
			    encoded, &layout, ucode_bit, 7u,
			    parameters->training_ucodes[segment]))
			return V90_DIL_INVALID_FRAME;
	}
	memcpy(descriptor, encoded, bytes);
	*payload_layout = layout;
	return V90_DIL_OK;
}

enum v90_dil_result v90_dil_encode(
	const struct v90_dil_parameters *parameters,
	uint8_t *descriptor, size_t descriptor_capacity,
	size_t *descriptor_bits)
{
	uint8_t encoded[V90_DIL_MAX_DESCRIPTOR_BYTES];
	struct pcm_dil_descriptor_layout layout;
	enum v90_dil_result result;

	if (descriptor == NULL || descriptor_bits == NULL)
		return V90_DIL_INVALID_ARGUMENT;
	result = v90_dil_payload_encode(
		parameters, encoded, sizeof(encoded), &layout);
	if (result != V90_DIL_OK)
		return result;
	if (descriptor_capacity < pcm_dil_descriptor_storage_bytes(&layout))
		return V90_DIL_INVALID_ARGUMENT;
	if (pcm_dil_descriptor_finalize(encoded, sizeof(encoded), &layout) !=
	    PCM_DIL_DESCRIPTOR_OK)
		return V90_DIL_INVALID_FRAME;
	memcpy(descriptor, encoded, pcm_dil_descriptor_storage_bytes(&layout));
	*descriptor_bits = layout.bit_count;
	return V90_DIL_OK;
}

static enum v90_dil_result common_result(
	enum pcm_dil_descriptor_result result)
{
	switch (result) {
	case PCM_DIL_DESCRIPTOR_OK:
		return V90_DIL_OK;
	case PCM_DIL_DESCRIPTOR_CRC_MISMATCH:
		return V90_DIL_CRC_MISMATCH;
	case PCM_DIL_DESCRIPTOR_INVALID_FRAME:
		return V90_DIL_INVALID_FRAME;
	case PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT:
		return V90_DIL_INVALID_ARGUMENT;
	}
	return V90_DIL_INVALID_FRAME;
}

static bool padding_is_zero(
	const uint8_t *descriptor, size_t first_bit, uint8_t pattern_bits)
{
	size_t padded_bits = ((pattern_bits + 15u) / 16u) * 16u;

	for (size_t bit = pattern_bits; bit < padded_bits; ++bit) {
		if (packed_msb_bit(
			    descriptor,
			    pattern_wire_bit(first_bit, bit)) != 0u)
			return false;
	}
	return true;
}

enum v90_dil_result v90_dil_decode(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t descriptor_bits, struct v90_dil_parameters *parameters)
{
	struct pcm_dil_descriptor_layout layout;
	enum pcm_dil_descriptor_result common;
	enum v90_dil_result layout_result;

	if (parameters == NULL)
		return V90_DIL_INVALID_ARGUMENT;
	layout_result = v90_dil_descriptor_layout(
		descriptor, descriptor_capacity, descriptor_bits, &layout);
	if (layout_result != V90_DIL_OK)
		return layout_result;
	if (layout.bit_count != descriptor_bits)
		return V90_DIL_INCOMPLETE;
	common = pcm_dil_descriptor_validate_framing(
		descriptor, descriptor_capacity, &layout);
	if (common != PCM_DIL_DESCRIPTOR_OK)
		return common_result(common);
	common = pcm_dil_descriptor_validate_crc(
		descriptor, descriptor_capacity, &layout);
	if (common != PCM_DIL_DESCRIPTOR_OK)
		return common_result(common);
	return v90_dil_payload_decode(
		descriptor, descriptor_capacity, parameters);
}

enum v90_dil_result v90_dil_payload_decode(
	const uint8_t *descriptor, size_t descriptor_capacity,
	struct v90_dil_parameters *parameters)
{
	struct pcm_dil_descriptor_layout layout;
	struct v90_dil_parameters decoded = { 0 };
	uint32_t value;
	size_t sign_span;
	size_t patterns_span;
	enum v90_dil_result result;

	if (parameters == NULL)
		return V90_DIL_INVALID_ARGUMENT;
	result = v90_dil_descriptor_layout(
		descriptor, descriptor_capacity, V90_DIL_HEADER_BITS, &layout);
	if (result != V90_DIL_OK)
		return result;
	if (descriptor_capacity < pcm_dil_descriptor_storage_bytes(&layout))
		return V90_DIL_INVALID_ARGUMENT;
	if (!read_field(
		    descriptor, descriptor_capacity, &layout,
		    N_FIRST_BIT, N_BITS, &value))
		return V90_DIL_INVALID_FRAME;
	decoded.segment_count = (uint8_t)value;
	if (!read_field(
		    descriptor, descriptor_capacity, &layout,
		    LSP_MINUS_ONE_FIRST_BIT, LENGTH_BITS, &value))
		return V90_DIL_INVALID_FRAME;
	decoded.sign_pattern_bits = (uint8_t)(value + 1u);
	if (!read_field(
		    descriptor, descriptor_capacity, &layout,
		    LTP_MINUS_ONE_FIRST_BIT, LENGTH_BITS, &value))
		return V90_DIL_INVALID_FRAME;
	decoded.training_pattern_bits = (uint8_t)(value + 1u);
	if (decoded.segment_count == 0u) {
		if (decoded.sign_pattern_bits != 1u ||
		    decoded.training_pattern_bits != 1u)
			return V90_DIL_INVALID_FRAME;
		*parameters = decoded;
		return V90_DIL_OK;
	}
	if (!padding_is_zero(
		    descriptor, PATTERN_DATA_FIRST_BIT,
		    decoded.sign_pattern_bits))
		return V90_DIL_INVALID_FRAME;
	for (size_t bit = 0; bit < decoded.sign_pattern_bits; ++bit)
		set_pattern_bit(
			decoded.sign_pattern, bit,
			packed_msb_bit(
				descriptor,
				pattern_wire_bit(
					PATTERN_DATA_FIRST_BIT, bit)) != 0u);
	sign_span = sign_pattern_span(decoded.sign_pattern_bits);
	if (!padding_is_zero(
		    descriptor, PATTERN_DATA_FIRST_BIT + sign_span,
		    decoded.training_pattern_bits))
		return V90_DIL_INVALID_FRAME;
	for (size_t bit = 0; bit < decoded.training_pattern_bits; ++bit)
		set_pattern_bit(
			decoded.training_pattern, bit,
			packed_msb_bit(
				descriptor,
				pattern_wire_bit(
					PATTERN_DATA_FIRST_BIT + sign_span,
					bit)) != 0u);
	patterns_span = sign_span +
		sign_pattern_span(decoded.training_pattern_bits);
	for (size_t chord = 0; chord < V90_DIL_CHORDS; ++chord) {
		size_t h_bit = fixed_pair_first_bit(
			patterns_span, chord / 2u) + (chord % 2u) * 8u;
		size_t reference_bit = fixed_pair_first_bit(
			patterns_span, 4u + chord / 2u) +
			(chord % 2u) * 8u;

		if (!read_field(
			    descriptor, descriptor_capacity, &layout,
			    h_bit, 7u, &value))
			return V90_DIL_INVALID_FRAME;
		decoded.segment_length_codes[chord] = (uint8_t)value;
		if (!read_field(
			    descriptor, descriptor_capacity, &layout,
			    reference_bit, 7u, &value))
			return V90_DIL_INVALID_FRAME;
		decoded.reference_ucodes[chord] = (uint8_t)value;
	}
	for (size_t segment = 0; segment < decoded.segment_count; ++segment) {
		size_t ucode_bit = fixed_pair_first_bit(
			patterns_span, 8u + segment / 2u) +
			(segment % 2u) * 8u;

		if (!read_field(
			    descriptor, descriptor_capacity, &layout,
			    ucode_bit, 7u, &value))
			return V90_DIL_INVALID_FRAME;
		decoded.training_ucodes[segment] = (uint8_t)value;
	}
	if (!v90_dil_parameters_are_valid(&decoded))
		return V90_DIL_INVALID_FRAME;
	*parameters = decoded;
	return V90_DIL_OK;
}

enum v90_dil_result v90_dil_encode_zero(
	uint8_t *descriptor, size_t descriptor_capacity)
{
	const struct v90_dil_parameters parameters = {
		.sign_pattern_bits = 1,
		.training_pattern_bits = 1,
	};
	size_t descriptor_bits;

	if (v90_dil_encode(
		    &parameters, descriptor, descriptor_capacity,
		    &descriptor_bits) != V90_DIL_OK)
		return V90_DIL_INVALID_ARGUMENT;
	return descriptor_bits == V90_DIL_ZERO_DESCRIPTOR_BITS ?
		       V90_DIL_OK : V90_DIL_INVALID_FRAME;
}

enum v90_dil_result v90_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity)
{
	struct v90_dil_parameters parameters;
	enum v90_dil_result result;

	if (descriptor == NULL ||
	    descriptor_capacity < V90_DIL_ZERO_DESCRIPTOR_BYTES)
		return V90_DIL_INVALID_ARGUMENT;
	if (initial_framing_is_valid(descriptor) &&
	    packed_msb_read_lsb_field(
		    descriptor, N_FIRST_BIT, N_BITS) != 0u)
		return V90_DIL_NONZERO_DESCRIPTOR;
	result = v90_dil_decode(
		descriptor, descriptor_capacity,
		V90_DIL_ZERO_DESCRIPTOR_BITS, &parameters);

	if (result == V90_DIL_INCOMPLETE)
		return V90_DIL_INVALID_FRAME;
	if (result != V90_DIL_OK)
		return result;
	return parameters.segment_count == 0u ?
		       V90_DIL_OK : V90_DIL_NONZERO_DESCRIPTOR;
}

const char *v90_dil_result_name(enum v90_dil_result result)
{
	switch (result) {
	case V90_DIL_OK:
		return "ok";
	case V90_DIL_INCOMPLETE:
		return "incomplete";
	case V90_DIL_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DIL_INVALID_FRAME:
		return "invalid frame";
	case V90_DIL_CRC_MISMATCH:
		return "CRC mismatch";
	case V90_DIL_NONZERO_DESCRIPTOR:
		return "nonzero descriptor";
	}
	return "unknown";
}
