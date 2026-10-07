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

static bool test_peer_descriptor(void)
{
	/* One complete Ja descriptor decoded from a peer call: only training
	 * parameters, not media or user payload. N=192, LSP=LTP=120, DRN 1..4. */
	static const uint8_t peer[] = {
		0xff, 0xff, 0x80, 0xc0, 0x1d, 0xdd, 0xc0, 0x00, 0x80, 0x00, 0x08, 0x00,
		0x00, 0x40, 0x1c, 0x38, 0xe3, 0x47, 0x03, 0x87, 0x1c, 0x58, 0xe0, 0x00,
		0x00, 0x80, 0x20, 0x08, 0x02, 0x00, 0x40, 0x3e, 0xff, 0xff, 0x7f, 0xff,
		0xbf, 0xff, 0xdf, 0xe0, 0x0d, 0xcd, 0xc7, 0x27, 0x23, 0x92, 0x41, 0x21,
		0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60,
		0xd0, 0xd0, 0x68, 0x68, 0x2c, 0x2c, 0x66, 0x66, 0x3d, 0x3d, 0x1e, 0x9e,
		0x8e, 0xce, 0xc2, 0x62, 0x61, 0xd1, 0xd0, 0xe8, 0xe8, 0x6c, 0x6c, 0x46,
		0x46, 0x2d, 0x2d, 0x16, 0x96, 0x8a, 0xca, 0xcf, 0xce, 0x27, 0x67, 0x53,
		0xab, 0xa9, 0xd5, 0xe4, 0x7c, 0x62, 0x36, 0x35, 0x1a, 0x9a, 0x8d, 0x4e,
		0x4b, 0xca, 0x25, 0x65, 0x52, 0xaa, 0xa9, 0x55, 0x64, 0x3c, 0x22, 0x16,
		0x15, 0x0a, 0x8a, 0x85, 0x46, 0x4d, 0xcc, 0x26, 0x66, 0x53, 0x2b, 0x29,
		0x95, 0xa4, 0x5c, 0x42, 0x26, 0x25, 0x12, 0x92, 0x89, 0x4a, 0x49, 0xc8,
		0x24, 0x64, 0x52, 0x2a, 0x29, 0x15, 0x24, 0x1c, 0x02, 0x06, 0x05, 0x02,
		0x82, 0x81, 0x42, 0x4f, 0x8f, 0x00, 0x70, 0x73, 0xd3, 0x99, 0xcd, 0xc8,
		0xfa, 0xfa, 0x7d, 0x7d, 0x12, 0x10, 0x1d, 0x1e, 0xcf, 0x6e, 0x03, 0xc3,
		0x82, 0x3a, 0x39, 0x71, 0x60, 0x4e, 0x4e, 0x14, 0x10, 0x2d, 0x29, 0x94,
		0xd4, 0x8b, 0xab, 0xa5, 0xd5, 0xd1, 0x51, 0x11, 0x51, 0x6c, 0xb6, 0xa0,
		0x1c, 0x18, 0x33, 0xb3, 0x9b, 0x1a, 0x02, 0xe2, 0xe6, 0xa6, 0x33, 0x1b,
		0x10, 0x10, 0xe8, 0xda, 0xda, 0x6d, 0x6d, 0x1a, 0x18, 0x19, 0x1a, 0xcd,
		0x6c, 0x02, 0xc2, 0x82, 0xba, 0xb8, 0x68, 0x48, 0x98, 0x90, 0x37, 0x37,
		0x25, 0x21, 0x90, 0xd0, 0x89, 0xa9, 0xa4, 0xd4, 0xd0, 0x50, 0x11, 0x11,
		0x2c, 0x96, 0x80, 0x0c, 0x08, 0x3b, 0xbb, 0x8c, 0x80, 0x0f, 0x00, 0x00,
		0x00, 0x00, 0x40, 0x90, 0x00,
	};
	struct v92_pcm_dil_parameters decoded, saved;
	struct pcm_dil_descriptor_layout layout;
	uint8_t frame[sizeof(peer)];
	size_t bits;

	CHECK(v92_pcm_dil_layout(peer, 7, 50, &layout) == V92_PCM_DIL_OK &&
	      layout.bit_count == 2148 && layout.crc_start_bit == 2125 &&
	      v92_pcm_dil_decode(peer, sizeof(peer), 2148, &decoded) == V92_PCM_DIL_OK &&
	      decoded.downstream.segment_count == 192 &&
	      decoded.downstream.sign_pattern_bits == 120 &&
	      decoded.downstream.training_pattern_bits == 120 &&
	      decoded.upstream_rate_mask == 0xf &&
	      v92_pcm_dil_encode(&decoded, frame, sizeof(frame), &bits) == V92_PCM_DIL_OK &&
	      bits == 2148 && memcmp(frame, peer, sizeof(peer)) == 0,
	      "V.92 peer's nonzero Ja descriptor did not reproduce exactly");
	saved = decoded;
	for (size_t bit = 0; bit < 2148; ++bit) {
		CHECK(v92_pcm_dil_decode(peer, sizeof(peer), bit, &decoded) ==
			      V92_PCM_DIL_INCOMPLETE &&
		      memcmp(&decoded, &saved, sizeof(decoded)) == 0,
		      "truncated V.92 descriptor changed output at bit %zu", bit);
		memcpy(frame, peer, sizeof(frame));
		assign_packed_bit(frame, bit, packed_bit(frame, bit) == 0u);
		CHECK(v92_pcm_dil_decode(frame, sizeof(frame), 2148, &decoded) !=
			      V92_PCM_DIL_OK &&
		      memcmp(&decoded, &saved, sizeof(decoded)) == 0,
		      "V.92 descriptor corruption was accepted at bit %zu", bit);
	}
	memcpy(frame, peer, sizeof(frame));
	bits = 123;
	CHECK(v92_pcm_dil_encode(&saved, frame, sizeof(frame) - 1u, &bits) ==
		      V92_PCM_DIL_INVALID_ARGUMENT && bits == 123 &&
	      memcmp(frame, peer, sizeof(frame)) == 0 &&
	      v92_pcm_dil_decode(peer, sizeof(peer) - 1u, 2148, &decoded) ==
		      V92_PCM_DIL_INVALID_ARGUMENT &&
	      v92_pcm_dil_decode(peer, sizeof(peer), 2149, &decoded) ==
		      V92_PCM_DIL_INVALID_FRAME &&
	      memcmp(&decoded, &saved, sizeof(decoded)) == 0,
	      "V.92 variable descriptor bounds were not transactional");
	return true;
}

static bool test_variable_descriptors(void)
{
	static const uint8_t segments[] = { 1, 2, 191, 192, 254, 255 };
	static const uint8_t lengths[] = { 1, 16, 17, 120, 128 };

	for (size_t n = 0; n < ARRAY_LENGTH(segments); ++n) {
		for (size_t s = 0; s < ARRAY_LENGTH(lengths); ++s) {
			for (size_t t = 0; t < ARRAY_LENGTH(lengths); ++t) {
				struct v92_pcm_dil_parameters source = { 0 }, decoded;
				struct v90_dil_parameters *dil = &source.downstream;
				struct pcm_dil_descriptor_layout layout;
				uint8_t frame[V92_PCM_DIL_MAX_DESCRIPTOR_BYTES + 1];
				size_t bits = 0;
				size_t crc = 221u +
					((lengths[s] + 15u) / 16u) * 17u +
					((lengths[t] + 15u) / 16u) * 17u +
					((segments[n] + 1u) / 2u) * 17u;

				dil->segment_count = segments[n];
				dil->sign_pattern_bits = lengths[s];
				dil->training_pattern_bits = lengths[t];
				for (size_t i = 0; i < lengths[s]; ++i)
					dil->sign_pattern[i / 8] |= (i % 3u == 0u) << (i % 8u);
				for (size_t i = 0; i < lengths[t]; ++i)
					dil->training_pattern[i / 8] |= (i % 5u == 0u) << (i % 8u);
				for (size_t i = 0; i < V90_DIL_CHORDS; ++i) {
					dil->segment_length_codes[i] = (uint8_t)(127u - i);
					dil->reference_ucodes[i] = (uint8_t)(i * 17u);
				}
				for (size_t i = 0; i < segments[n]; ++i)
					dil->training_ucodes[i] = (uint8_t)((i * 11u) % 128u);
				source.upstream_rate_mask = V92_PCM_DIL_RATE_MASK_ALL;
				memset(frame, 0xa5, sizeof(frame));
				CHECK(v92_pcm_dil_encode(&source, frame, sizeof(frame), &bits) ==
					      V92_PCM_DIL_OK && bits == ((crc + 29u) / 12u) * 12u &&
				      frame[(bits + 7u) / 8u] == 0xa5 &&
				      v92_pcm_dil_layout(frame, 7, V90_DIL_HEADER_BITS, &layout) ==
					      V92_PCM_DIL_OK && layout.crc_start_bit == crc &&
				      layout.bit_count == bits &&
				      v92_pcm_dil_decode(frame, (bits + 7u) / 8u, bits, &decoded) ==
					      V92_PCM_DIL_OK &&
				      memcmp(&decoded.downstream, dil, sizeof(*dil)) == 0 &&
				      decoded.upstream_rate_mask == source.upstream_rate_mask,
				      "V.92 descriptor N=%u LSP=%u LTP=%u did not round trip",
				      segments[n], lengths[s], lengths[t]);
				/* The high word's unused 13 bits are CRC-protected but ignored. */
				assign_packed_bit(frame, crc - 1u, true);
				CHECK(pcm_dil_descriptor_finalize(frame, sizeof(frame), &layout) ==
					      PCM_DIL_DESCRIPTOR_OK &&
				      v92_pcm_dil_decode(frame, sizeof(frame), bits, &decoded) ==
					      V92_PCM_DIL_OK &&
				      decoded.upstream_rate_mask == source.upstream_rate_mask,
				      "V.92 nonzero descriptor interpreted reserved mask bits");
			}
		}
	}
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

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream DIL descriptors\n");
	if (!test_generated_masks() || !test_rate_masks() ||
	    !test_variable_descriptors() || !test_peer_descriptor() ||
	    !test_reserved_bits() || !test_rejection()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream DIL descriptors\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream DIL descriptors\n");
	return 0;
}
