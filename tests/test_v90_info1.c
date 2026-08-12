// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_info.h>
#include <v90_info1.h>

#include "test.h"

#include <string.h>

static struct v90_info1a selection(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 37,
		.training_ucode = 78,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3200,
		.frequency_offset_1_50_hz_units = -137,
	};
}

static bool selections_equal(
	const struct v90_info1a *left, const struct v90_info1a *right)
{
	return left->local_md_length_35_ms_units ==
		       right->local_md_length_35_ms_units &&
	       left->training_ucode == right->training_ucode &&
	       left->upstream_symbol_rate == right->upstream_symbol_rate &&
	       left->frequency_offset_1_50_hz_units ==
		       right->frequency_offset_1_50_hz_units;
}

static bool generate_frame(
	const uint8_t *payload, uint8_t *frame, size_t frame_capacity)
{
	struct v34_phase2_info_generator generator;
	size_t frame_bits = v34_phase2_info_frame_bit_count(
		V90_INFO1A_PAYLOAD_BITS);

	CHECK(frame_bits == 70 && frame_capacity >= (frame_bits + 7u) / 8u &&
	      v34_phase2_info_generator_init(
		      &generator, payload, V90_INFO1A_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_OK,
	      "V.90 INFO1a frame fixture did not initialize");
	memset(frame, 0, frame_capacity);
	for (size_t index = 0; index < frame_bits; ++index) {
		uint8_t bit;
		bool complete;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      V34_PHASE2_INFO_OK &&
		      complete == (index + 1u == frame_bits),
		      "V.90 INFO1a frame generation failed at bit %zu",
		      index);
		frame[index / 8u] |=
			(uint8_t)(bit << (7u - index % 8u));
	}
	return true;
}

static bool test_wire_image(void)
{
	static const uint8_t expected_payload[] = {
		0x02, 0x93, 0x90, 0xbe, 0xec,
	};
	static const uint8_t expected_frame[] = {
		0xf7, 0x20, 0x29, 0x39, 0x0b, 0xee, 0xd6, 0x17, 0x7c,
	};
	struct v90_info1a selected = selection();
	struct v90_info1a decoded;
	uint8_t payload[V90_INFO1A_PAYLOAD_BYTES];
	uint8_t frame[sizeof(expected_frame)];

	CHECK(V90_INFO1A_PAYLOAD_BITS == 38 &&
	      V90_INFO1A_PAYLOAD_BYTES == sizeof(expected_payload) &&
	      v90_info1a_is_valid(&selected) &&
	      v90_info1a_encode(&selected, payload, sizeof(payload)) ==
		      V90_INFO1_OK &&
	      memcmp(payload, expected_payload, sizeof(payload)) == 0 &&
	      v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &decoded) == V90_INFO1_OK &&
	      selections_equal(&selected, &decoded) &&
	      generate_frame(payload, frame, sizeof(frame)) &&
	      memcmp(frame, expected_frame, sizeof(frame)) == 0,
	      "V.90 INFO1a wire image changed");
	return true;
}

static bool test_reserved_bits(void)
{
	struct v90_info1a selected = selection();
	struct v90_info1a decoded;
	uint8_t payload[V90_INFO1A_PAYLOAD_BYTES];

	CHECK(v90_info1a_encode(&selected, payload, sizeof(payload)) ==
		      V90_INFO1_OK,
	      "reserved-bit fixture did not encode");
	payload[0] |= UINT8_C(0xfc);
	payload[2] |= UINT8_C(0x0c);
	CHECK(v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &decoded) == V90_INFO1_OK &&
	      selections_equal(&selected, &decoded),
	      "reserved INFO1a bits were interpreted");
	CHECK(v90_info1a_encode(&decoded, payload, sizeof(payload)) ==
		      V90_INFO1_OK &&
	      (payload[0] & UINT8_C(0xfc)) == 0 &&
	      (payload[2] & UINT8_C(0x0c)) == 0,
	      "INFO1a encoding did not canonicalize reserved bits");
	return true;
}

static void replace_field(
	uint8_t *payload, size_t first_bit, size_t bit_count, uint16_t value)
{
	for (size_t index = 0; index < bit_count; ++index) {
		uint8_t mask = (uint8_t)(
			1u << (7u - (first_bit + index) % 8u));

		payload[(first_bit + index) / 8u] &= (uint8_t)~mask;
		if ((value & (uint16_t)(1u << index)) != 0)
			payload[(first_bit + index) / 8u] |= mask;
	}
}

static bool test_validation(void)
{
	struct v90_info1a valid = selection();
	struct v90_info1a invalid = valid;
	struct v90_info1a decoded = {
		.local_md_length_35_ms_units = 1,
		.training_ucode = 67,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3000,
	};
	struct v90_info1a saved_decoded = decoded;
	uint8_t payload[V90_INFO1A_PAYLOAD_BYTES + 1u] = {
		0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	};
	uint8_t saved_payload[sizeof(payload)];

	memcpy(saved_payload, payload, sizeof(payload));
	invalid.training_ucode = V90_INFO1A_MIN_TRAINING_UCODE - 1u;
	CHECK(!v90_info1a_is_valid(&invalid) &&
	      v90_info1a_encode(&invalid, payload, sizeof(payload)) ==
		      V90_INFO1_INVALID_SELECTION &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO1a accepted an invalid training Ucode or changed output");
	invalid = valid;
	invalid.upstream_symbol_rate = V34_SYMBOL_RATE_2800;
	CHECK(!v90_info1a_is_valid(&invalid) &&
	      v90_info1a_encode(&invalid, payload, sizeof(payload)) ==
		      V90_INFO1_INVALID_SELECTION &&
	      v90_info1a_encode(NULL, payload, sizeof(payload)) ==
		      V90_INFO1_INVALID_ARGUMENT &&
	      v90_info1a_encode(&valid, NULL, sizeof(payload)) ==
		      V90_INFO1_INVALID_ARGUMENT &&
	      v90_info1a_encode(
		      &valid, payload, V90_INFO1A_PAYLOAD_BYTES - 1u) ==
		      V90_INFO1_INVALID_ARGUMENT &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO1a encoding accepted an invalid argument or selection");
	CHECK(v90_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V90_INFO1_OK,
	      "decode fixture did not encode");
	replace_field(payload, 25, 3, 5);
	CHECK(v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &decoded) == V90_INFO1_INVALID_PAYLOAD &&
	      selections_equal(&decoded, &saved_decoded),
	      "INFO1a accepted a non-V.90 downstream symbol rate");
	CHECK(v90_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V90_INFO1_OK,
	      "decode fixture did not reset");
	replace_field(payload, 13, 7, 66);
	CHECK(v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &decoded) == V90_INFO1_INVALID_PAYLOAD &&
	      selections_equal(&decoded, &saved_decoded),
	      "INFO1a accepted an out-of-range training Ucode");
	CHECK(v90_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V90_INFO1_OK,
	      "padding fixture did not reset");
	payload[V90_INFO1A_PAYLOAD_BYTES - 1u] |= 1u;
	CHECK(v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &decoded) == V90_INFO1_INVALID_PAYLOAD &&
	      v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS - 1u,
		      &decoded) == V90_INFO1_INVALID_PAYLOAD &&
	      v90_info1a_decode(
		      NULL, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &decoded) == V90_INFO1_INVALID_ARGUMENT &&
	      v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      NULL) == V90_INFO1_INVALID_ARGUMENT &&
	      v90_info1a_decode(
		      payload, V90_INFO1A_PAYLOAD_BYTES - 1u,
		      V90_INFO1A_PAYLOAD_BITS, &decoded) ==
		      V90_INFO1_INVALID_ARGUMENT &&
	      selections_equal(&decoded, &saved_decoded) &&
	      !v90_info1a_is_valid(NULL) &&
	      strcmp(v90_info1_result_name(V90_INFO1_INVALID_PAYLOAD),
		     "invalid payload") == 0 &&
	      strcmp(v90_info1_result_name(
		     (enum v90_info1_result)4), "unknown") == 0,
	      "INFO1a decoding accepted malformed input or changed output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire image", test_wire_image },
		{ "reserved bits", test_reserved_bits },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
