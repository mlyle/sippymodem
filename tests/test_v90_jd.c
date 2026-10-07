// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v90_jd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	REFERENCE_CRC_POLYNOMIAL = 0x1021,
	REFERENCE_CRC_INITIAL = 0xffff,
	FIRST_INFORMATION_BIT = 18,
	FIRST_START_BIT = 17,
	FIELD_STRIDE_BITS = 17,
	CRC_FIRST_BIT = 52,
	CRC_BITS = 16,
};

static struct v90_jd_parameters parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = UINT32_C(0x2a5a5a),
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 3,
	};
}

static bool parameters_equal(
	const struct v90_jd_parameters *left,
	const struct v90_jd_parameters *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static uint8_t reference_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >> (7u - index % 8u)) & 1u);
}

static void replace_bit(uint8_t *frame, size_t index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - index % 8u));

	frame[index / 8u] &= (uint8_t)~mask;
	if (bit != 0)
		frame[index / 8u] |= mask;
}

static bool reference_start_bit(size_t index)
{
	return index >= FIRST_START_BIT && index < CRC_FIRST_BIT &&
	       (index - FIRST_START_BIT) % FIELD_STRIDE_BITS == 0;
}

static uint16_t reference_crc(const uint8_t *frame)
{
	uint16_t remainder = REFERENCE_CRC_INITIAL;

	for (size_t index = FIRST_INFORMATION_BIT;
	     index < CRC_FIRST_BIT; ++index) {
		uint16_t next;

		if (reference_start_bit(index))
			continue;
		next = (uint16_t)(remainder << 1);
		if ((((remainder >> 15) ^ reference_bit(frame, index)) & 1u) !=
		    0)
			next ^= REFERENCE_CRC_POLYNOMIAL;
		remainder = next;
	}
	return remainder;
}

static void replace_crc(uint8_t *frame)
{
	uint16_t remainder = reference_crc(frame);

	for (size_t index = 0; index < CRC_BITS; ++index)
		replace_bit(
			frame, CRC_FIRST_BIT + index,
			(uint8_t)(remainder >> (CRC_BITS - index - 1u)) & 1u);
}

static bool test_wire_image(void)
{
	static const uint8_t expected[V90_JD_FRAME_BYTES] = {
		0xff, 0xff, 0x96, 0x96, 0x8a, 0x81, 0x67, 0x34, 0xf0,
	};
	struct v90_jd_parameters selected = parameters();
	struct v90_jd_parameters decoded;
	uint8_t frame[V90_JD_FRAME_BYTES];

	CHECK(V90_JD_FRAME_BITS == 72 && V90_JD_FRAME_BYTES == 9 &&
	      V90_JD_RATE_CAPABILITY_BITS == 22 &&
	      v90_jd_parameters_are_valid(&selected) &&
	      v90_jd_encode(&selected, frame, sizeof(frame)) == V90_JD_OK &&
	      memcmp(frame, expected, sizeof(frame)) == 0 &&
	      reference_crc(frame) ==
		      (uint16_t)((uint16_t)frame[6] << 12 |
				 (uint16_t)frame[7] << 4 |
				 (uint16_t)frame[8] >> 4) &&
	      v90_jd_decode(frame, sizeof(frame), &decoded) == V90_JD_OK &&
	      parameters_equal(&selected, &decoded),
	      "V.90 Jd wire image changed");
	return true;
}

static bool test_reserved_bits(void)
{
	struct v90_jd_parameters selected = parameters();
	struct v90_jd_parameters decoded;
	uint8_t frame[V90_JD_FRAME_BYTES];
	uint8_t canonical[V90_JD_FRAME_BYTES];

	CHECK(v90_jd_encode(&selected, frame, sizeof(frame)) == V90_JD_OK,
	      "reserved-bit fixture did not encode");
	memcpy(canonical, frame, sizeof(frame));
	for (size_t index = 41; index <= 46; ++index)
		replace_bit(frame, index, (uint8_t)(index & 1u));
	replace_crc(frame);
	CHECK(v90_jd_decode(frame, sizeof(frame), &decoded) == V90_JD_OK &&
	      parameters_equal(&selected, &decoded) &&
	      v90_jd_encode(&decoded, frame, sizeof(frame)) == V90_JD_OK &&
	      memcmp(frame, canonical, sizeof(frame)) == 0,
	      "Jd reserved bits were interpreted or not canonicalized");
	return true;
}

static bool test_frame_rejection(void)
{
	struct v90_jd_parameters selected = parameters();
	struct v90_jd_parameters decoded = {
		.data_rate_capability_mask = 1,
		.training_constellation = V34_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 1,
	};
	struct v90_jd_parameters saved = decoded;
	uint8_t frame[V90_JD_FRAME_BYTES];
	uint8_t malformed[V90_JD_FRAME_BYTES];

	CHECK(v90_jd_encode(&selected, frame, sizeof(frame)) == V90_JD_OK,
	      "rejection fixture did not encode");
	memcpy(malformed, frame, sizeof(malformed));
	replace_bit(malformed, 18, reference_bit(malformed, 18) ^ 1u);
	CHECK(v90_jd_decode(malformed, sizeof(malformed), &decoded) ==
		      V90_JD_CRC_MISMATCH &&
	      parameters_equal(&decoded, &saved),
	      "Jd accepted a corrupt information bit");
	for (size_t index = 0; index < 4; ++index) {
		static const size_t framing_bits[] = { 0, 17, 34, 68 };

		memcpy(malformed, frame, sizeof(malformed));
		replace_bit(
			malformed, framing_bits[index],
			reference_bit(malformed, framing_bits[index]) ^ 1u);
		CHECK(v90_jd_decode(malformed, sizeof(malformed), &decoded) ==
			      V90_JD_INVALID_FRAME &&
		      parameters_equal(&decoded, &saved),
		      "Jd accepted malformed framing bit %zu",
		      framing_bits[index]);
	}
	memcpy(malformed, frame, sizeof(malformed));
	replace_bit(malformed, 49, 0);
	replace_bit(malformed, 50, 0);
	replace_crc(malformed);
	CHECK(v90_jd_decode(malformed, sizeof(malformed), &decoded) ==
		      V90_JD_INVALID_FRAME &&
	      parameters_equal(&decoded, &saved),
	      "Jd accepted zero maximum lookahead");
	return true;
}

static bool test_validation(void)
{
	struct v90_jd_parameters valid = parameters();
	struct v90_jd_parameters invalid = valid;
	struct v90_jd_parameters decoded = valid;
	uint8_t frame[V90_JD_FRAME_BYTES + 1u];
	uint8_t saved_frame[sizeof(frame)];

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	invalid.data_rate_capability_mask =
		UINT32_C(1) << V90_JD_RATE_CAPABILITY_BITS;
	CHECK(!v90_jd_parameters_are_valid(&invalid) &&
	      v90_jd_encode(&invalid, frame, sizeof(frame)) ==
		      V90_JD_INVALID_PARAMETERS,
	      "Jd accepted an out-of-range rate mask");
	invalid = valid;
	invalid.training_constellation = (enum v34_constellation_size)8;
	CHECK(!v90_jd_parameters_are_valid(&invalid) &&
	      v90_jd_encode(&invalid, frame, sizeof(frame)) ==
		      V90_JD_INVALID_PARAMETERS,
	      "Jd accepted an invalid training constellation");
	invalid = valid;
	invalid.maximum_spectral_shaping_lookahead = 4;
	CHECK(!v90_jd_parameters_are_valid(&invalid) &&
	      v90_jd_encode(&invalid, frame, sizeof(frame)) ==
		      V90_JD_INVALID_PARAMETERS &&
	      v90_jd_encode(NULL, frame, sizeof(frame)) ==
		      V90_JD_INVALID_ARGUMENT &&
	      v90_jd_encode(&valid, NULL, sizeof(frame)) ==
		      V90_JD_INVALID_ARGUMENT &&
	      v90_jd_encode(&valid, frame, V90_JD_FRAME_BYTES - 1u) ==
		      V90_JD_INVALID_ARGUMENT &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid Jd encoding changed output");
	CHECK(v90_jd_decode(NULL, V90_JD_FRAME_BYTES, &decoded) ==
		      V90_JD_INVALID_ARGUMENT &&
	      v90_jd_decode(frame, V90_JD_FRAME_BYTES, NULL) ==
		      V90_JD_INVALID_ARGUMENT &&
	      v90_jd_decode(frame, V90_JD_FRAME_BYTES - 1u, &decoded) ==
		      V90_JD_INVALID_ARGUMENT &&
	      parameters_equal(&decoded, &valid) &&
	      !v90_jd_parameters_are_valid(NULL),
	      "Jd API validation changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "wire image", test_wire_image },
		{ "reserved bits", test_reserved_bits },
		{ "frame rejection", test_frame_rejection },
		{ "validation and transactionality", test_validation },
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
