// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_dil.h"

#include "pcm_dil_descriptor.h"
#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	N_FIRST_BIT = 18,
	N_BITS = 8,
	RATE_MASK_LOW_BITS = 16,
	RATE_MASK_HIGH_BITS = 16,
	RATE_MASK_SPAN_BITS = 2 * PCM_DIL_FIELD_STRIDE_BITS,
	CRC_START_BIT = 255,
};

static const struct pcm_dil_descriptor_layout zero_layout = {
	.crc_start_bit = CRC_START_BIT,
	.bit_count = V92_PCM_DIL_ZERO_DESCRIPTOR_BITS,
};

bool v92_pcm_dil_rate_mask_is_valid(uint32_t rate_mask)
{
	return (rate_mask & ~V92_PCM_DIL_RATE_MASK_ALL) == 0u;
}

bool v92_pcm_dil_rate_is_enabled(
	uint32_t rate_mask, uint8_t data_rate_number)
{
	return v92_pcm_dil_rate_mask_is_valid(rate_mask) &&
	       data_rate_number >= V92_PCM_DIL_MIN_DATA_RATE_NUMBER &&
	       data_rate_number <= V92_PCM_DIL_MAX_DATA_RATE_NUMBER &&
	       (rate_mask & (UINT32_C(1) << (data_rate_number - 1u))) != 0u;
}

bool v92_pcm_dil_rate_mask_through(
	uint8_t maximum_data_rate_number, uint32_t *rate_mask)
{
	uint32_t generated;

	if (rate_mask == NULL ||
	    maximum_data_rate_number > V92_PCM_DIL_MAX_DATA_RATE_NUMBER)
		return false;
	generated = maximum_data_rate_number == 0u ?
			    0u :
			    (UINT32_C(1) << maximum_data_rate_number) - 1u;
	*rate_mask = generated;
	return true;
}

static void extend_layout(struct pcm_dil_descriptor_layout *layout)
{
	layout->crc_start_bit += RATE_MASK_SPAN_BITS;
	layout->bit_count = layout->crc_start_bit + PCM_DIL_CRC_BITS + 2u;
	layout->bit_count = ((layout->bit_count + 11u) / 12u) * 12u;
}

static enum v92_pcm_dil_result payload_result(enum v90_dil_result result)
{
	switch (result) {
	case V90_DIL_OK:
		return V92_PCM_DIL_OK;
	case V90_DIL_INVALID_ARGUMENT:
		return V92_PCM_DIL_INVALID_ARGUMENT;
	case V90_DIL_INCOMPLETE:
		return V92_PCM_DIL_INCOMPLETE;
	default:
		return V92_PCM_DIL_INVALID_FRAME;
	}
}

enum v92_pcm_dil_result v92_pcm_dil_layout(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t received_bits, struct pcm_dil_descriptor_layout *layout)
{
	struct pcm_dil_descriptor_layout generated;
	enum v90_dil_result result;

	if (descriptor == NULL || layout == NULL ||
	    received_bits > V92_PCM_DIL_MAX_DESCRIPTOR_BITS ||
	    descriptor_capacity < (received_bits + 7u) / 8u)
		return V92_PCM_DIL_INVALID_ARGUMENT;
	if (received_bits < V90_DIL_HEADER_BITS)
		return V92_PCM_DIL_INCOMPLETE;
	result = v90_dil_descriptor_layout(
		descriptor, descriptor_capacity, V90_DIL_HEADER_BITS, &generated);
	if (result != V90_DIL_OK)
		return payload_result(result);
	extend_layout(&generated);
	if (received_bits > generated.bit_count ||
	    !pcm_dil_descriptor_layout_is_valid(&generated))
		return V92_PCM_DIL_INVALID_FRAME;
	*layout = generated;
	return V92_PCM_DIL_OK;
}

enum v92_pcm_dil_result v92_pcm_dil_encode(
	const struct v92_pcm_dil_parameters *parameters,
	uint8_t *descriptor, size_t descriptor_capacity,
	size_t *descriptor_bits)
{
	uint8_t encoded[V92_PCM_DIL_MAX_DESCRIPTOR_BYTES] = { 0 };
	struct pcm_dil_descriptor_layout layout;
	enum v90_dil_result result;
	size_t mask_start;
	size_t bytes;

	if (parameters == NULL || descriptor == NULL ||
	    descriptor_bits == NULL ||
	    !v92_pcm_dil_rate_mask_is_valid(parameters->upstream_rate_mask))
		return V92_PCM_DIL_INVALID_ARGUMENT;
	result = v90_dil_payload_encode(
		&parameters->downstream, encoded, sizeof(encoded), &layout);
	if (result != V90_DIL_OK)
		return payload_result(result);
	mask_start = layout.crc_start_bit + 1u;
	extend_layout(&layout);
	bytes = pcm_dil_descriptor_storage_bytes(&layout);
	if (bytes == 0u || descriptor_capacity < bytes)
		return V92_PCM_DIL_INVALID_ARGUMENT;
	if (!pcm_dil_descriptor_write_lsb_field(
		    encoded, sizeof(encoded), &layout, mask_start,
		    RATE_MASK_LOW_BITS,
		    parameters->upstream_rate_mask & UINT32_C(0xffff)) ||
	    !pcm_dil_descriptor_write_lsb_field(
		    encoded, sizeof(encoded), &layout,
		    mask_start + PCM_DIL_FIELD_STRIDE_BITS, RATE_MASK_HIGH_BITS,
		    parameters->upstream_rate_mask >> RATE_MASK_LOW_BITS) ||
	    pcm_dil_descriptor_finalize(
		    encoded, sizeof(encoded), &layout) !=
		    PCM_DIL_DESCRIPTOR_OK)
		return V92_PCM_DIL_INVALID_FRAME;
	memcpy(descriptor, encoded, bytes);
	*descriptor_bits = layout.bit_count;
	return V92_PCM_DIL_OK;
}

enum v92_pcm_dil_result v92_pcm_dil_decode(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t descriptor_bits, struct v92_pcm_dil_parameters *parameters)
{
	struct pcm_dil_descriptor_layout layout;
	struct v92_pcm_dil_parameters decoded = { 0 };
	enum v92_pcm_dil_result result;
	enum pcm_dil_descriptor_result common;
	uint32_t low_rates;
	uint32_t high_rates;
	size_t mask_start;

	if (parameters == NULL)
		return V92_PCM_DIL_INVALID_ARGUMENT;
	result = v92_pcm_dil_layout(
		descriptor, descriptor_capacity, descriptor_bits, &layout);
	if (result != V92_PCM_DIL_OK)
		return result;
	if (descriptor_bits != layout.bit_count)
		return V92_PCM_DIL_INCOMPLETE;
	common = pcm_dil_descriptor_validate_crc(
		descriptor, descriptor_capacity, &layout);
	if (common != PCM_DIL_DESCRIPTOR_OK)
		return common == PCM_DIL_DESCRIPTOR_CRC_MISMATCH ?
			       V92_PCM_DIL_CRC_MISMATCH :
			       V92_PCM_DIL_INVALID_FRAME;
	result = payload_result(v90_dil_payload_decode(
		descriptor, descriptor_capacity, &decoded.downstream));
	if (result != V92_PCM_DIL_OK)
		return result;
	mask_start = layout.crc_start_bit - RATE_MASK_SPAN_BITS + 1u;
	low_rates = packed_msb_read_lsb_field(
		descriptor, mask_start, RATE_MASK_LOW_BITS);
	high_rates = packed_msb_read_lsb_field(
		descriptor, mask_start + PCM_DIL_FIELD_STRIDE_BITS,
		RATE_MASK_HIGH_BITS);
	decoded.upstream_rate_mask =
		(low_rates | (high_rates << RATE_MASK_LOW_BITS)) &
		V92_PCM_DIL_RATE_MASK_ALL;
	*parameters = decoded;
	return V92_PCM_DIL_OK;
}

enum v92_pcm_dil_result v92_pcm_dil_encode_zero(
	uint32_t upstream_rate_mask,
	uint8_t *descriptor, size_t descriptor_capacity)
{
	const struct v92_pcm_dil_parameters parameters = {
		.downstream = { .sign_pattern_bits = 1, .training_pattern_bits = 1 },
		.upstream_rate_mask = upstream_rate_mask,
	};
	size_t bits;

	return v92_pcm_dil_encode(
		&parameters, descriptor, descriptor_capacity, &bits);
}

enum v92_pcm_dil_result v92_pcm_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity,
	uint32_t *upstream_rate_mask)
{
	struct v92_pcm_dil_parameters decoded;
	enum v92_pcm_dil_result result;

	if (descriptor == NULL || upstream_rate_mask == NULL ||
	    descriptor_capacity < V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES)
		return V92_PCM_DIL_INVALID_ARGUMENT;
	if (pcm_dil_descriptor_validate_framing(
		    descriptor, descriptor_capacity, &zero_layout) !=
	    PCM_DIL_DESCRIPTOR_OK)
		return V92_PCM_DIL_INVALID_FRAME;
	if (packed_msb_read_lsb_field(descriptor, N_FIRST_BIT, N_BITS) != 0u)
		return V92_PCM_DIL_NONZERO_DESCRIPTOR;
	result = v92_pcm_dil_decode(
		descriptor, descriptor_capacity, V92_PCM_DIL_ZERO_DESCRIPTOR_BITS,
		&decoded);
	if (result != V92_PCM_DIL_OK)
		return result == V92_PCM_DIL_INCOMPLETE ?
			V92_PCM_DIL_INVALID_FRAME : result;
	*upstream_rate_mask = decoded.upstream_rate_mask;
	return V92_PCM_DIL_OK;
}

const char *v92_pcm_dil_result_name(enum v92_pcm_dil_result result)
{
	switch (result) {
	case V92_PCM_DIL_OK:
		return "ok";
	case V92_PCM_DIL_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_DIL_INVALID_FRAME:
		return "invalid frame";
	case V92_PCM_DIL_CRC_MISMATCH:
		return "CRC mismatch";
	case V92_PCM_DIL_NONZERO_DESCRIPTOR:
		return "nonzero descriptor";
	case V92_PCM_DIL_INCOMPLETE:
		return "incomplete";
	}
	return "unknown";
}
