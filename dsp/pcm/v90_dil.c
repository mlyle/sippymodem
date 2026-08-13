// SPDX-License-Identifier: BSD-3-Clause

#include "v90_dil.h"

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
	CRC_START_BIT = 221,
};

static const struct pcm_dil_descriptor_layout zero_layout = {
	.crc_start_bit = CRC_START_BIT,
	.bit_count = V90_DIL_ZERO_DESCRIPTOR_BITS,
};

static bool read_field(
	const uint8_t *descriptor, size_t first_bit,
	unsigned int field_bits, uint32_t *value)
{
	return pcm_dil_descriptor_read_lsb_field(
		descriptor, V90_DIL_ZERO_DESCRIPTOR_BYTES,
		&zero_layout, first_bit, field_bits, value);
}

static bool zero_lengths_are_valid(const uint8_t *descriptor)
{
	uint32_t lsp_minus_one;
	uint32_t ltp_minus_one;

	return read_field(
		       descriptor, LSP_MINUS_ONE_FIRST_BIT, LENGTH_BITS,
		       &lsp_minus_one) &&
	       read_field(
		       descriptor, LTP_MINUS_ONE_FIRST_BIT, LENGTH_BITS,
		       &ltp_minus_one) &&
	       lsp_minus_one == 0u && ltp_minus_one == 0u;
}

enum v90_dil_result v90_dil_encode_zero(
	uint8_t *descriptor, size_t descriptor_capacity)
{
	uint8_t encoded[V90_DIL_ZERO_DESCRIPTOR_BYTES] = { 0 };

	if (descriptor == NULL || descriptor_capacity < sizeof(encoded))
		return V90_DIL_INVALID_ARGUMENT;
	if (pcm_dil_descriptor_initialize(
		    encoded, sizeof(encoded), &zero_layout) !=
		    PCM_DIL_DESCRIPTOR_OK ||
	    pcm_dil_descriptor_finalize(
		    encoded, sizeof(encoded), &zero_layout) !=
		    PCM_DIL_DESCRIPTOR_OK)
		return V90_DIL_INVALID_FRAME;
	memcpy(descriptor, encoded, sizeof(encoded));
	return V90_DIL_OK;
}

enum v90_dil_result v90_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity)
{
	enum pcm_dil_descriptor_result framing;
	uint32_t n;

	if (descriptor == NULL ||
	    descriptor_capacity < V90_DIL_ZERO_DESCRIPTOR_BYTES)
		return V90_DIL_INVALID_ARGUMENT;
	framing = pcm_dil_descriptor_validate_framing(
		descriptor, V90_DIL_ZERO_DESCRIPTOR_BYTES, &zero_layout);
	if (framing != PCM_DIL_DESCRIPTOR_OK)
		return V90_DIL_INVALID_FRAME;
	if (!read_field(descriptor, N_FIRST_BIT, N_BITS, &n))
		return V90_DIL_INVALID_FRAME;
	if (n != 0u)
		return V90_DIL_NONZERO_DESCRIPTOR;
	if (!zero_lengths_are_valid(descriptor))
		return V90_DIL_INVALID_FRAME;
	if (pcm_dil_descriptor_validate_crc(
		    descriptor, V90_DIL_ZERO_DESCRIPTOR_BYTES,
		    &zero_layout) != PCM_DIL_DESCRIPTOR_OK)
		return V90_DIL_CRC_MISMATCH;
	return V90_DIL_OK;
}

const char *v90_dil_result_name(enum v90_dil_result result)
{
	switch (result) {
	case V90_DIL_OK:
		return "ok";
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
