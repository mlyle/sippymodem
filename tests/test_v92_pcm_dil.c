// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_dil.h>

#include <pcm_dil_descriptor.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	RATE_MASK_LOW_FIRST_BIT = 222,
	RATE_MASK_HIGH_FIRST_BIT = 239,
	RATE_MASK_RESERVED_FIRST_BIT = 242,
	RATE_MASK_RESERVED_END_BIT = 255,
	CRC_START_BIT = 255,
	FILL_FIRST_BIT = 272,
};

static const struct pcm_dil_descriptor_layout zero_layout = {
	.crc_start_bit = CRC_START_BIT,
	.bit_count = V92_PCM_DIL_ZERO_DESCRIPTOR_BITS,
};

static uint8_t packed_bit(const uint8_t *descriptor, size_t bit)
{
	return (uint8_t)((descriptor[bit / 8u] >>
			  (7u - bit % 8u)) &
			  1u);
}

static void assign_packed_bit(
	uint8_t *descriptor, size_t bit, bool value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit % 8u));

	if (value)
		descriptor[bit / 8u] |= mask;
	else
		descriptor[bit / 8u] &= (uint8_t)~mask;
}

static uint32_t packed_lsb_field(
	const uint8_t *descriptor, size_t first_bit,
	unsigned int field_bits)
{
	uint32_t value = 0u;

	for (unsigned int bit = 0; bit < field_bits; ++bit)
		value |= (uint32_t)packed_bit(descriptor, first_bit + bit) << bit;
	return value;
}

static bool finalize(uint8_t *descriptor)
{
	return pcm_dil_descriptor_finalize(
		       descriptor, V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES,
		       &zero_layout) == PCM_DIL_DESCRIPTOR_OK;
}

static bool check_mask(uint32_t rate_mask)
{
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES + 2u];
	uint32_t decoded = UINT32_MAX;

	memset(descriptor, 0xa5, sizeof(descriptor));
	CHECK(v92_pcm_dil_encode_zero(
		      rate_mask, descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK &&
	      descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES] == 0xa5u &&
	      descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES + 1u] == 0xa5u &&
	      (descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES - 1u] & 0x0fu) ==
		      0u &&
	      packed_lsb_field(descriptor, RATE_MASK_LOW_FIRST_BIT, 16u) ==
		      (rate_mask & UINT32_C(0xffff)) &&
	      packed_lsb_field(descriptor, RATE_MASK_HIGH_FIRST_BIT, 16u) ==
		      (rate_mask >> 16u),
	      "V.92 zero-DIL mask 0x%05x encoded incorrectly",
	      (unsigned int)rate_mask);
	for (size_t bit = RATE_MASK_RESERVED_FIRST_BIT;
	     bit < RATE_MASK_RESERVED_END_BIT; ++bit)
		CHECK(packed_bit(descriptor, bit) == 0u,
		      "V.92 zero-DIL reserved bit %zu was not zero", bit);
	CHECK(v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_OK &&
	      decoded == rate_mask,
	      "V.92 zero-DIL mask 0x%05x did not round trip",
	      (unsigned int)rate_mask);
	return true;
}

static bool test_generated_masks(void)
{
	static const uint32_t masks[] = {
		0u,
		1u,
		UINT32_C(1) << (V92_PCM_DIL_MAX_DATA_RATE_NUMBER - 1u),
		UINT32_C(0x55555),
		V92_PCM_DIL_RATE_MASK_ALL,
	};

	CHECK(V92_PCM_DIL_ZERO_DESCRIPTOR_BITS == 276 &&
	      V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES == 35,
	      "V.92 zero-DIL dimensions changed");
	for (size_t index = 0; index < ARRAY_LENGTH(masks); ++index)
		CHECK(check_mask(masks[index]),
		      "V.92 zero-DIL generated case %zu failed", index);
	return true;
}

static bool test_rate_masks(void)
{
	uint32_t mask = UINT32_MAX;
	uint32_t unchanged;

	CHECK(v92_pcm_dil_rate_mask_is_valid(0u) &&
	      v92_pcm_dil_rate_mask_is_valid(V92_PCM_DIL_RATE_MASK_ALL) &&
	      !v92_pcm_dil_rate_mask_is_valid(
		      UINT32_C(1) << V92_PCM_DIL_MAX_DATA_RATE_NUMBER) &&
	      v92_pcm_dil_rate_mask_through(0u, &mask) && mask == 0u &&
	      v92_pcm_dil_rate_mask_through(1u, &mask) && mask == 1u &&
	      v92_pcm_dil_rate_mask_through(
		      V92_PCM_DIL_MAX_DATA_RATE_NUMBER, &mask) &&
	      mask == V92_PCM_DIL_RATE_MASK_ALL,
	      "V.92 rate-mask construction failed");
	for (uint8_t drn = V92_PCM_DIL_MIN_DATA_RATE_NUMBER;
	     drn <= V92_PCM_DIL_MAX_DATA_RATE_NUMBER; ++drn)
		CHECK(v92_pcm_dil_rate_is_enabled(mask, drn),
		      "V.92 data rate number %u was not enabled", drn);
	unchanged = mask;
	CHECK(!v92_pcm_dil_rate_is_enabled(mask, 0u) &&
	      !v92_pcm_dil_rate_is_enabled(
		      mask, V92_PCM_DIL_MAX_DATA_RATE_NUMBER + 1u) &&
	      !v92_pcm_dil_rate_is_enabled(
		      UINT32_C(1) << V92_PCM_DIL_MAX_DATA_RATE_NUMBER, 1u) &&
	      !v92_pcm_dil_rate_mask_through(
		      V92_PCM_DIL_MAX_DATA_RATE_NUMBER + 1u, &mask) &&
	      mask == unchanged &&
	      !v92_pcm_dil_rate_mask_through(1u, NULL),
	      "invalid V.92 rate-mask access was accepted");
	return true;
}

static bool test_reserved_bits(void)
{
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	uint32_t decoded = UINT32_MAX;
	const uint32_t expected = UINT32_C(0x4567);

	CHECK(v92_pcm_dil_encode_zero(
		      expected, descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK,
	      "V.92 reserved-bit fixture did not encode");
	assign_packed_bit(descriptor, 26u, true);
	assign_packed_bit(descriptor, RATE_MASK_RESERVED_FIRST_BIT, true);
	CHECK(finalize(descriptor) &&
	      v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_OK &&
	      decoded == expected,
	      "V.92 decoder interpreted reserved descriptor bits");
	return true;
}

static bool test_rejection(void)
{
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	uint8_t saved[sizeof(descriptor)];
	uint32_t decoded = UINT32_C(0xa5a5a5a5);
	const uint32_t decoded_before = decoded;

	memset(descriptor, 0xa5, sizeof(descriptor));
	memcpy(saved, descriptor, sizeof(descriptor));
	CHECK(v92_pcm_dil_encode_zero(
		      UINT32_C(1) << V92_PCM_DIL_MAX_DATA_RATE_NUMBER,
		      descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      v92_pcm_dil_encode_zero(1u, NULL, sizeof(descriptor)) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      v92_pcm_dil_encode_zero(
		      1u, descriptor, sizeof(descriptor) - 1u) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      memcmp(descriptor, saved, sizeof(descriptor)) == 0,
	      "invalid V.92 zero-DIL encoding changed output");

	CHECK(v92_pcm_dil_encode_zero(
		      UINT32_C(0x12345), descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK,
	      "V.92 rejection fixture did not encode");
	assign_packed_bit(
		descriptor, RATE_MASK_LOW_FIRST_BIT,
		packed_bit(descriptor, RATE_MASK_LOW_FIRST_BIT) == 0u);
	CHECK(v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_CRC_MISMATCH &&
	      decoded == decoded_before,
	      "V.92 zero-DIL CRC failure changed output");

	CHECK(v92_pcm_dil_encode_zero(
		      1u, descriptor, sizeof(descriptor)) == V92_PCM_DIL_OK,
	      "V.92 nonzero-DIL fixture did not reset");
	assign_packed_bit(descriptor, 18u, true);
	CHECK(finalize(descriptor) &&
	      v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_NONZERO_DESCRIPTOR &&
	      decoded == decoded_before,
	      "V.92 N>0 descriptor was not classified transactionally");

	CHECK(v92_pcm_dil_encode_zero(
		      1u, descriptor, sizeof(descriptor)) == V92_PCM_DIL_OK,
	      "V.92 malformed-length fixture did not reset");
	assign_packed_bit(descriptor, 35u, true);
	CHECK(finalize(descriptor) &&
	      v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_INVALID_FRAME &&
	      decoded == decoded_before,
	      "V.92 zero-DIL accepted a nonzero learning length");

	CHECK(v92_pcm_dil_encode_zero(
		      1u, descriptor, sizeof(descriptor)) == V92_PCM_DIL_OK,
	      "V.92 malformed-framing fixture did not reset");
	assign_packed_bit(descriptor, 238u, true);
	CHECK(v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_INVALID_FRAME,
	      "V.92 zero-DIL accepted a nonzero start bit");
	assign_packed_bit(descriptor, 238u, false);
	assign_packed_bit(descriptor, FILL_FIRST_BIT, true);
	CHECK(v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_INVALID_FRAME &&
	      v92_pcm_dil_decode_zero(NULL, sizeof(descriptor), &decoded) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor) - 1u, &decoded) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      v92_pcm_dil_decode_zero(
		      descriptor, sizeof(descriptor), NULL) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      decoded == decoded_before,
	      "V.92 zero-DIL malformed/API rejection changed output");
	return true;
}

static bool test_result_names(void)
{
	static const char *const names[] = {
		"ok", "invalid argument", "invalid frame", "CRC mismatch",
		"nonzero descriptor",
	};

	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_dil_result_name(
				      (enum v92_pcm_dil_result)index),
			      names[index]) == 0,
		      "V.92 DIL result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_dil_result_name((enum v92_pcm_dil_result)-1),
		      "unknown") == 0,
	      "unknown V.92 DIL result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream zero-DIL descriptor\n");
	if (!test_generated_masks() || !test_rate_masks() ||
	    !test_reserved_bits() || !test_rejection() ||
	    !test_result_names()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream zero-DIL descriptor\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream zero-DIL descriptor\n");
	return 0;
}
