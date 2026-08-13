// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_dil.h"

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
	RATE_MASK_LOW_FIRST_BIT = 222,
	RATE_MASK_LOW_BITS = 16,
	RATE_MASK_HIGH_FIRST_BIT = 239,
	RATE_MASK_HIGH_BITS = 16,
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

static bool read_field(
	const uint8_t *descriptor, size_t first_bit,
	unsigned int field_bits, uint32_t *value)
{
	return pcm_dil_descriptor_read_lsb_field(
		descriptor, V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES,
		&zero_layout, first_bit, field_bits, value);
}

static bool write_field(
	uint8_t *descriptor, size_t first_bit,
	unsigned int field_bits, uint32_t value)
{
	return pcm_dil_descriptor_write_lsb_field(
		descriptor, V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES,
		&zero_layout, first_bit, field_bits, value);
}

enum v92_pcm_dil_result v92_pcm_dil_encode_zero(
	uint32_t upstream_rate_mask,
	uint8_t *descriptor, size_t descriptor_capacity)
{
	uint8_t encoded[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];

	if (!v92_pcm_dil_rate_mask_is_valid(upstream_rate_mask) ||
	    descriptor == NULL || descriptor_capacity < sizeof(encoded))
		return V92_PCM_DIL_INVALID_ARGUMENT;
	if (pcm_dil_descriptor_initialize(
		    encoded, sizeof(encoded), &zero_layout) !=
		    PCM_DIL_DESCRIPTOR_OK ||
	    !write_field(
		    encoded, RATE_MASK_LOW_FIRST_BIT, RATE_MASK_LOW_BITS,
		    upstream_rate_mask & UINT32_C(0xffff)) ||
	    !write_field(
		    encoded, RATE_MASK_HIGH_FIRST_BIT, RATE_MASK_HIGH_BITS,
		    upstream_rate_mask >> RATE_MASK_LOW_BITS) ||
	    pcm_dil_descriptor_finalize(
		    encoded, sizeof(encoded), &zero_layout) !=
		    PCM_DIL_DESCRIPTOR_OK)
		return V92_PCM_DIL_INVALID_FRAME;
	memcpy(descriptor, encoded, sizeof(encoded));
	return V92_PCM_DIL_OK;
}

enum v92_pcm_dil_result v92_pcm_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity,
	uint32_t *upstream_rate_mask)
{
	enum pcm_dil_descriptor_result common;
	uint32_t n;
	uint32_t lsp_minus_one;
	uint32_t ltp_minus_one;
	uint32_t low_rates;
	uint32_t high_rates;
	uint32_t decoded;

	if (descriptor == NULL || upstream_rate_mask == NULL ||
	    descriptor_capacity < V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES)
		return V92_PCM_DIL_INVALID_ARGUMENT;
	common = pcm_dil_descriptor_validate_framing(
		descriptor, V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES, &zero_layout);
	if (common != PCM_DIL_DESCRIPTOR_OK)
		return V92_PCM_DIL_INVALID_FRAME;
	if (!read_field(descriptor, N_FIRST_BIT, N_BITS, &n) ||
	    !read_field(
		    descriptor, LSP_MINUS_ONE_FIRST_BIT, LENGTH_BITS,
		    &lsp_minus_one) ||
	    !read_field(
		    descriptor, LTP_MINUS_ONE_FIRST_BIT, LENGTH_BITS,
		    &ltp_minus_one) ||
	    !read_field(
		    descriptor, RATE_MASK_LOW_FIRST_BIT, RATE_MASK_LOW_BITS,
		    &low_rates) ||
	    !read_field(
		    descriptor, RATE_MASK_HIGH_FIRST_BIT, RATE_MASK_HIGH_BITS,
		    &high_rates))
		return V92_PCM_DIL_INVALID_FRAME;
	if (n != 0u)
		return V92_PCM_DIL_NONZERO_DESCRIPTOR;
	if (lsp_minus_one != 0u || ltp_minus_one != 0u)
		return V92_PCM_DIL_INVALID_FRAME;
	common = pcm_dil_descriptor_validate_crc(
		descriptor, V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES, &zero_layout);
	if (common != PCM_DIL_DESCRIPTOR_OK)
		return common == PCM_DIL_DESCRIPTOR_CRC_MISMATCH ?
			       V92_PCM_DIL_CRC_MISMATCH :
			       V92_PCM_DIL_INVALID_FRAME;
	decoded = (low_rates | (high_rates << RATE_MASK_LOW_BITS)) &
		  V92_PCM_DIL_RATE_MASK_ALL;
	*upstream_rate_mask = decoded;
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
	}
	return "unknown";
}
