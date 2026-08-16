// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v90_dil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	REFERENCE_CRC_POLYNOMIAL = 0x8408,
	REFERENCE_CRC_INITIAL = 0xffff,
	FIRST_START_BIT = 17,
	FIRST_INFORMATION_BIT = 18,
	CRC_START_BIT = 221,
	CRC_FIRST_BIT = 222,
	CRC_BITS = 16,
	FIELD_STRIDE_BITS = 17,
};

static uint8_t bit_at(const uint8_t *descriptor, size_t index)
{
	return (uint8_t)((descriptor[index / 8u] >>
			  (7u - index % 8u)) &
			 1u);
}

static void replace_bit(uint8_t *descriptor, size_t index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - index % 8u));

	descriptor[index / 8u] &= (uint8_t)~mask;
	if (bit != 0)
		descriptor[index / 8u] |= mask;
}

static bool is_start_bit(size_t index)
{
	return index >= FIRST_START_BIT && index <= CRC_START_BIT &&
	       (index - FIRST_START_BIT) % FIELD_STRIDE_BITS == 0;
}

static uint16_t reference_crc(const uint8_t *descriptor)
{
	uint16_t remainder = REFERENCE_CRC_INITIAL;

	for (size_t index = FIRST_INFORMATION_BIT;
	     index < CRC_START_BIT; ++index) {
		uint16_t feedback;

		if (is_start_bit(index))
			continue;
		feedback = (uint16_t)((remainder ^ bit_at(descriptor, index)) & 1u);
		remainder = (uint16_t)(remainder >> 1);
		if (feedback != 0)
			remainder ^= REFERENCE_CRC_POLYNOMIAL;
	}
	return remainder;
}

static void replace_crc(uint8_t *descriptor)
{
	uint16_t remainder = reference_crc(descriptor);

	for (size_t index = 0; index < CRC_BITS; ++index)
		replace_bit(
			descriptor, CRC_FIRST_BIT + index,
			(uint8_t)(remainder >> index) & 1u);
}

static bool test_wire_image(void)
{
	static const uint8_t expected[V90_DIL_ZERO_DESCRIPTOR_BYTES] = {
		0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x03, 0xbe, 0x28,
	};
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];

	CHECK(V90_DIL_ZERO_DESCRIPTOR_BITS == 240 &&
	      V90_DIL_ZERO_DESCRIPTOR_BYTES == 30 &&
	      v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK &&
	      memcmp(descriptor, expected, sizeof(descriptor)) == 0 &&
	      reference_crc(descriptor) == UINT16_C(0x51f7) &&
	      v90_dil_decode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "canonical zero-DIL descriptor changed");
	return true;
}

static bool test_ignored_values(void)
{
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	uint8_t canonical[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	static const size_t ignored_bits[] = {
		26, 33, 42, 50, 52, 67, 69, 84, 86, 92,
		93, 154, 160, 161, 205, 220,
	};

	CHECK(v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "ignored-value fixture did not encode");
	memcpy(canonical, descriptor, sizeof(canonical));
	for (size_t index = 0;
	     index < sizeof(ignored_bits) / sizeof(ignored_bits[0]); ++index)
		replace_bit(descriptor, ignored_bits[index], 1);
	replace_crc(descriptor);
	CHECK(v90_dil_decode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "zero-DIL decoder interpreted unused parameter values");
	CHECK(v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK &&
	      memcmp(descriptor, canonical, sizeof(descriptor)) == 0,
	      "zero-DIL encoder did not restore its canonical image");
	return true;
}

static bool test_rejection(void)
{
	static const size_t framing_bits[] = {
		0, 17, 34, 221, 238, 239,
	};
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];

	CHECK(v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "rejection fixture did not encode");
	replace_bit(descriptor, 52, 1);
	CHECK(v90_dil_decode_zero(descriptor, sizeof(descriptor)) ==
		      V90_DIL_CRC_MISMATCH,
	      "zero-DIL decoder accepted a damaged information bit");

	CHECK(v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "framing fixture did not encode");
	for (size_t index = 0; index < ARRAY_LENGTH(framing_bits); ++index) {
		uint8_t malformed[V90_DIL_ZERO_DESCRIPTOR_BYTES];

		memcpy(malformed, descriptor, sizeof(malformed));
		replace_bit(
			malformed, framing_bits[index],
			bit_at(malformed, framing_bits[index]) ^ 1u);
		CHECK(v90_dil_decode_zero(malformed, sizeof(malformed)) ==
			      V90_DIL_INVALID_FRAME,
		      "zero-DIL decoder accepted framing bit %zu",
		      framing_bits[index]);
	}

	replace_bit(descriptor, 18, 1);
	CHECK(v90_dil_decode_zero(descriptor, sizeof(descriptor)) ==
		      V90_DIL_NONZERO_DESCRIPTOR,
	      "zero-DIL decoder did not classify a nonzero descriptor header");

	CHECK(v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "zero-length fixture did not encode");
	replace_bit(descriptor, 35, 1);
	replace_crc(descriptor);
	CHECK(v90_dil_decode_zero(descriptor, sizeof(descriptor)) ==
		      V90_DIL_INVALID_FRAME,
	      "zero-DIL decoder accepted a nonzero LSP length");
	CHECK(v90_dil_encode_zero(descriptor, sizeof(descriptor)) == V90_DIL_OK,
	      "second zero-length fixture did not encode");
	replace_bit(descriptor, 43, 1);
	replace_crc(descriptor);
	CHECK(v90_dil_decode_zero(descriptor, sizeof(descriptor)) ==
		      V90_DIL_INVALID_FRAME,
	      "zero-DIL decoder accepted a nonzero LTP length");
	return true;
}

static void set_pattern_bit(uint8_t *pattern, size_t bit)
{
	pattern[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static struct v90_dil_parameters make_parameters(
	uint8_t segments, uint8_t sign_bits, uint8_t training_bits)
{
	struct v90_dil_parameters parameters = {
		.segment_count = segments,
		.sign_pattern_bits = sign_bits,
		.training_pattern_bits = training_bits,
	};

	for (size_t bit = 0; bit < sign_bits; ++bit) {
		if ((bit * 5u + 1u) % 7u < 3u)
			set_pattern_bit(parameters.sign_pattern, bit);
	}
	for (size_t bit = 0; bit < training_bits; ++bit) {
		if ((bit * 3u + 2u) % 11u < 5u)
			set_pattern_bit(parameters.training_pattern, bit);
	}
	for (size_t chord = 0; chord < V90_DIL_CHORDS; ++chord) {
		parameters.segment_length_codes[chord] =
			(uint8_t)(chord * 13u + 3u);
		parameters.reference_ucodes[chord] =
			(uint8_t)(chord * 16u + 7u);
	}
	for (size_t segment = 0; segment < segments; ++segment)
		parameters.training_ucodes[segment] =
			(uint8_t)((segment * 29u + 11u) & 0x7fu);
	return parameters;
}

static size_t expected_descriptor_bits(
	uint8_t segments, uint8_t sign_bits, uint8_t training_bits)
{
	size_t alpha = ((sign_bits + 15u) / 16u) * 17u;
	size_t beta = alpha + ((training_bits + 15u) / 16u) * 17u;
	size_t crc_start = 187u + beta + ((segments + 1u) / 2u) * 17u;
	size_t bits = crc_start + 18u;

	return bits + bits % 2u;
}

static bool run_variable_case(
	uint8_t segments, uint8_t sign_bits, uint8_t training_bits)
{
	struct v90_dil_parameters parameters = make_parameters(
		segments, sign_bits, training_bits);
	struct v90_dil_parameters decoded;
	struct pcm_dil_descriptor_layout layout = { 0 };
	uint8_t descriptor[V90_DIL_MAX_DESCRIPTOR_BYTES];
	size_t bits = SIZE_MAX;
	size_t expected = expected_descriptor_bits(
		segments, sign_bits, training_bits);

	memset(descriptor, 0xa5, sizeof(descriptor));
	memset(&decoded, 0xa5, sizeof(decoded));
	CHECK(v90_dil_parameters_are_valid(&parameters) &&
	      v90_dil_encode(
		      &parameters, descriptor, sizeof(descriptor), &bits) ==
		      V90_DIL_OK &&
	      bits == expected &&
	      v90_dil_descriptor_layout(
		      descriptor, sizeof(descriptor), V90_DIL_HEADER_BITS - 1u,
		      &layout) == V90_DIL_INCOMPLETE &&
	      layout.bit_count == 0u &&
	      v90_dil_descriptor_layout(
		      descriptor, sizeof(descriptor), V90_DIL_HEADER_BITS,
		      &layout) == V90_DIL_OK &&
	      layout.bit_count == expected &&
	      v90_dil_decode(
		      descriptor, sizeof(descriptor), bits, &decoded) ==
		      V90_DIL_OK &&
	      memcmp(&decoded, &parameters, sizeof(decoded)) == 0,
	      "variable DIL %u/%u/%u did not round trip (%zu/%zu bits)",
	      segments, sign_bits, training_bits, bits, expected);
	for (size_t bit = 0; bit < V90_DIL_FRAME_SYNC_BITS; ++bit)
		CHECK(bit_at(descriptor, bit) == 1u,
		      "variable DIL frame sync bit %zu differed", bit);
	for (size_t bit = 17u; bit <= layout.crc_start_bit; bit += 17u)
		CHECK(bit_at(descriptor, bit) == 0u,
		      "variable DIL start bit %zu differed", bit);
	return true;
}

static bool test_variable_descriptors(void)
{
	static const struct {
		uint8_t segments;
		uint8_t sign_bits;
		uint8_t training_bits;
	} cases[] = {
		{ 1, 1, 1 },
		{ 3, 17, 33 },
		{ 144, 120, 120 },
		{ 255, 128, 128 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(run_variable_case(
			      cases[index].segments,
			      cases[index].sign_bits,
			      cases[index].training_bits),
		      "variable DIL case %zu failed", index);
	return true;
}

static bool test_variable_rejection(void)
{
	struct v90_dil_parameters parameters = make_parameters(3, 17, 33);
	struct v90_dil_parameters decoded;
	struct pcm_dil_descriptor_layout layout;
	uint8_t descriptor[V90_DIL_MAX_DESCRIPTOR_BYTES];
	uint8_t saved[V90_DIL_MAX_DESCRIPTOR_BYTES];
	size_t bits;

	CHECK(v90_dil_encode(
		      &parameters, descriptor, sizeof(descriptor), &bits) ==
		      V90_DIL_OK &&
	      v90_dil_descriptor_layout(
		      descriptor, sizeof(descriptor), bits, &layout) ==
		      V90_DIL_OK,
	      "variable rejection fixture did not initialize");
	replace_bit(descriptor, 69u + 1u, 1u);
	CHECK(pcm_dil_descriptor_finalize(
		      descriptor, sizeof(descriptor), &layout) ==
		      PCM_DIL_DESCRIPTOR_OK &&
	      v90_dil_decode(
		      descriptor, sizeof(descriptor), bits, &decoded) ==
		      V90_DIL_INVALID_FRAME,
	      "variable DIL accepted nonzero pattern padding");

	parameters.training_ucodes[parameters.segment_count] = 1u;
	memset(descriptor, 0xa5, sizeof(descriptor));
	memcpy(saved, descriptor, sizeof(saved));
	bits = SIZE_MAX;
	CHECK(!v90_dil_parameters_are_valid(&parameters) &&
	      v90_dil_encode(
		      &parameters, descriptor, sizeof(descriptor), &bits) ==
		      V90_DIL_INVALID_ARGUMENT &&
	      memcmp(descriptor, saved, sizeof(descriptor)) == 0 &&
	      bits == SIZE_MAX,
	      "invalid variable DIL changed its outputs");
	return true;
}

static bool test_api(void)
{
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	uint8_t saved[V90_DIL_ZERO_DESCRIPTOR_BYTES];

	memset(descriptor, 0xa5, sizeof(descriptor));
	memcpy(saved, descriptor, sizeof(saved));
	CHECK(v90_dil_encode_zero(NULL, sizeof(descriptor)) ==
		      V90_DIL_INVALID_ARGUMENT &&
	      v90_dil_encode_zero(descriptor, sizeof(descriptor) - 1u) ==
		      V90_DIL_INVALID_ARGUMENT &&
	      memcmp(descriptor, saved, sizeof(descriptor)) == 0 &&
	      v90_dil_decode_zero(NULL, sizeof(descriptor)) ==
		      V90_DIL_INVALID_ARGUMENT &&
	      v90_dil_decode_zero(descriptor, sizeof(descriptor) - 1u) ==
		      V90_DIL_INVALID_ARGUMENT,
	      "zero-DIL API boundary was not transactional");
	CHECK(strcmp(v90_dil_result_name(V90_DIL_OK), "ok") == 0 &&
	      strcmp(v90_dil_result_name(V90_DIL_NONZERO_DESCRIPTOR),
		     "nonzero descriptor") == 0 &&
	      strcmp(v90_dil_result_name((enum v90_dil_result)99), "unknown") == 0,
	      "zero-DIL result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "canonical wire image", test_wire_image },
		{ "ignored zero-DIL values", test_ignored_values },
		{ "framing and CRC rejection", test_rejection },
		{ "variable descriptor round trips", test_variable_descriptors },
		{ "variable descriptor rejection", test_variable_rejection },
		{ "API and transactionality", test_api },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
