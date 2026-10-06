// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_info.h>
#include <v90_info0.h>

#include "test.h"

#include <string.h>

static struct v34_info0 common_capabilities(bool acknowledged)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.constellation_1664_supported = true,
		.acknowledges_peer_info0 = acknowledged,
	};
}

static bool generate_frame(
	const uint8_t *payload, size_t payload_bits,
	uint8_t *frame, size_t frame_capacity)
{
	struct v34_phase2_info_generator generator;
	size_t frame_bits = v34_phase2_info_frame_bit_count(payload_bits);

	CHECK(frame_bits != 0 && frame_capacity >= (frame_bits + 7u) / 8u &&
	      v34_phase2_info_generator_init(
		      &generator, payload, payload_bits) ==
		      V34_PHASE2_INFO_OK,
	      "V.90 INFO0 frame fixture did not initialize");
	memset(frame, 0, frame_capacity);
	for (size_t index = 0; index < frame_bits; ++index) {
		uint8_t bit;
		bool complete;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      V34_PHASE2_INFO_OK &&
		      complete == (index + 1u == frame_bits),
		      "V.90 INFO0 frame generation failed at bit %zu", index);
		frame[index / 8u] |=
			(uint8_t)(bit << (7u - index % 8u));
	}
	return true;
}

static bool test_wire_images(void)
{
	static const uint8_t expected_a_payload[] = { 0xff, 0x84, 0x00 };
	static const uint8_t expected_a_frame[] = {
		0xf7, 0x2f, 0xf8, 0x44, 0x7e, 0xcf, 0x80,
	};
	static const uint8_t expected_d_payload[] = {
		0xff, 0x84, 0x4a, 0x78,
	};
	static const uint8_t expected_d_frame[] = {
		0xf7, 0x2f, 0xf8, 0x44, 0xa7, 0x92, 0x54, 0x3c,
	};
	struct v34_info0 analogue = common_capabilities(false);
	struct v90_info0d digital = {
		.common = common_capabilities(false),
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = V90_PCM_A_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
	struct v34_info0 decoded_a;
	struct v90_info0d decoded_d;
	uint8_t a_payload[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t d_payload[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t a_frame[sizeof(expected_a_frame)];
	uint8_t d_frame[sizeof(expected_d_frame)];

	CHECK(v90_info0a_encode(&analogue, a_payload, sizeof(a_payload)) ==
		      V90_INFO0_OK &&
	      memcmp(a_payload, expected_a_payload, sizeof(a_payload)) == 0 &&
	      v90_info0a_decode(
		      a_payload, sizeof(a_payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded_a) == V90_INFO0_OK &&
	      v90_info0a_equal(&analogue, &decoded_a) &&
	      generate_frame(
		      a_payload, V90_INFO0A_PAYLOAD_BITS,
		      a_frame, sizeof(a_frame)) &&
	      memcmp(a_frame, expected_a_frame, sizeof(a_frame)) == 0,
	      "INFO0a wire image changed");
	CHECK(v90_info0d_encode(&digital, d_payload, sizeof(d_payload)) ==
		      V90_INFO0_OK &&
	      memcmp(d_payload, expected_d_payload, sizeof(d_payload)) == 0 &&
	      v90_info0d_decode(
		      d_payload, sizeof(d_payload), V90_INFO0D_PAYLOAD_BITS,
		      &decoded_d) == V90_INFO0_OK &&
	      v90_info0d_equal(&digital, &decoded_d) &&
	      generate_frame(
		      d_payload, V90_INFO0D_PAYLOAD_BITS,
		      d_frame, sizeof(d_frame)) &&
	      memcmp(d_frame, expected_d_frame, sizeof(d_frame)) == 0,
	      "INFO0d wire image changed");
	return true;
}

static bool test_shared_v34_capabilities(void)
{
	struct v34_info0 source = common_capabilities(true), decoded;
	uint8_t pcm[V90_INFO0A_PAYLOAD_BYTES], expected[sizeof(pcm)];
	uint8_t ordinary[V34_INFO0_PAYLOAD_BYTES];

	CHECK(v90_info0a_encode(&source, expected, sizeof(expected)) == V90_INFO0_OK,
	      "shared capability fixture failed");
	for (unsigned clock = 0; clock < 4; ++clock) {
		source.transmit_clock_source = (enum v34_info0_clock_source)clock;
		CHECK(v90_info0a_encode(&source, pcm, sizeof(pcm)) == V90_INFO0_OK &&
		      memcmp(pcm, expected, sizeof(pcm)) == 0 &&
		      (unsigned)source.transmit_clock_source == clock,
		      "V.90 used or changed the V.34-only clock field");
		if (clock == 3)
			continue;
		CHECK(v34_info0_encode(&source, ordinary, sizeof(ordinary)) == V34_INFO0_OK &&
		      v90_info0a_decode(ordinary, sizeof(ordinary), V90_INFO0A_PAYLOAD_BITS,
			&decoded) == V90_INFO0_OK && v90_info0a_equal(&source, &decoded) &&
		      decoded.transmit_clock_source == V34_INFO0_CLOCK_INTERNAL,
		      "V.90 interpreted received reserved clock bits");
	}
	return true;
}

static bool test_comparison(void)
{
	struct v90_info0d source = { .common = common_capabilities(false) }, decoded;
	uint8_t original[V90_INFO0D_PAYLOAD_BYTES], mutated[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t canonical[V90_INFO0D_PAYLOAD_BYTES];

	CHECK(!v90_info0a_equal(NULL, &source.common) && !v90_info0d_equal(&source, NULL) &&
	      v90_info0d_encode(&source, original, sizeof(original)) == V90_INFO0_OK,
	      "INFO0 comparison fixture did not encode or accepted null");
	for (size_t bit = 0; bit < V90_INFO0D_PAYLOAD_BITS; ++bit) {
		memcpy(mutated, original, sizeof(mutated));
		mutated[bit / 8u] ^= (uint8_t)(1u << (7u - bit % 8u));
		CHECK(v90_info0d_decode(mutated, sizeof(mutated), V90_INFO0D_PAYLOAD_BITS,
			      &decoded) == V90_INFO0_OK &&
		      v90_info0d_encode(&decoded, canonical, sizeof(canonical)) == V90_INFO0_OK &&
		      v90_info0d_equal(&source, &decoded) ==
			      (memcmp(original, canonical, sizeof(original)) == 0),
		      "INFO0 comparison differs from encoded fields at bit %zu", bit);
	}
	return true;
}

static bool test_reserved_bits(void)
{
	struct v34_info0 analogue = common_capabilities(true);
	struct v90_info0d digital = {
		.common = common_capabilities(true),
		.nominal_transmit_power = 15,
		.maximum_transmit_power = 31,
		.pcm_law = V90_PCM_MU_LAW,
	};
	struct v34_info0 decoded_a;
	struct v90_info0d decoded_d;
	uint8_t a_payload[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t d_payload[V90_INFO0D_PAYLOAD_BYTES];

	CHECK(v90_info0a_encode(&analogue, a_payload, sizeof(a_payload)) ==
		      V90_INFO0_OK &&
	      v90_info0d_encode(&digital, d_payload, sizeof(d_payload)) ==
		      V90_INFO0_OK,
	      "reserved-bit fixtures did not encode");
	a_payload[1] |= UINT8_C(0x03);
	d_payload[1] |= UINT8_C(0x03);
	d_payload[3] |= UINT8_C(0x04);
	CHECK(v90_info0a_decode(
		      a_payload, sizeof(a_payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded_a) == V90_INFO0_OK &&
	      v90_info0a_equal(&decoded_a, &analogue) &&
	      v90_info0d_decode(
		      d_payload, sizeof(d_payload), V90_INFO0D_PAYLOAD_BITS,
		      &decoded_d) == V90_INFO0_OK &&
	      v90_info0d_equal(&decoded_d, &digital),
	      "V.90 reserved bits were interpreted");
	CHECK(v90_info0a_encode(&decoded_a, a_payload, sizeof(a_payload)) ==
		      V90_INFO0_OK &&
	      (a_payload[1] & UINT8_C(0x03)) == 0 &&
	      v90_info0d_encode(&decoded_d, d_payload, sizeof(d_payload)) ==
		      V90_INFO0_OK &&
	      (d_payload[1] & UINT8_C(0x03)) == 0 &&
	      (d_payload[3] & UINT8_C(0x04)) == 0,
	      "V.90 encoders did not canonicalize reserved bits");
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 valid = common_capabilities(false);
	struct v34_info0 invalid = valid;
	struct v34_info0 decoded = common_capabilities(true);
	struct v34_info0 saved_decoded = decoded;
	struct v90_info0d valid_d = {
		.common = valid,
		.pcm_law = V90_PCM_MU_LAW,
	};
	uint8_t payload[] = { 0xff, 0x84, 0x00, 0xa5 };
	uint8_t saved_payload[ARRAY_LENGTH(payload)];

	memcpy(saved_payload, payload, sizeof(payload));
	invalid.maximum_symbol_rate_difference_steps =
		V90_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS + 1u;
	CHECK(!v90_info0_is_valid(&invalid) && !v90_info0a_equal(&invalid, &invalid) &&
	      v90_info0a_encode(&invalid, payload, sizeof(payload)) ==
		      V90_INFO0_INVALID_CAPABILITIES &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO0a accepted invalid capabilities or mutated output");
	valid_d.pcm_law = (enum v90_pcm_law)2;
	CHECK(!v90_info0d_is_valid(&valid_d) &&
	      v90_info0d_encode(&valid_d, payload, sizeof(payload)) ==
		      V90_INFO0_INVALID_CAPABILITIES &&
	      v90_info0a_encode(NULL, payload, sizeof(payload)) ==
		      V90_INFO0_INVALID_ARGUMENT &&
	      v90_info0a_encode(&valid, NULL, sizeof(payload)) ==
		      V90_INFO0_INVALID_ARGUMENT &&
	      v90_info0a_encode(
		      &valid, payload, V90_INFO0A_PAYLOAD_BYTES - 1u) ==
		      V90_INFO0_INVALID_ARGUMENT &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO0 encoding accepted invalid arguments or mutated output");
	CHECK(v90_info0a_decode(
		      NULL, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded) == V90_INFO0_INVALID_ARGUMENT &&
	      v90_info0a_decode(
		      payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      NULL) == V90_INFO0_INVALID_ARGUMENT &&
	      v90_info0a_decode(
		      payload, V90_INFO0A_PAYLOAD_BYTES - 1u,
		      V90_INFO0A_PAYLOAD_BITS, &decoded) ==
		      V90_INFO0_INVALID_ARGUMENT &&
	      v90_info0a_decode(
		      payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS - 1u,
		      &decoded) == V90_INFO0_INVALID_PAYLOAD &&
	      v90_info0a_equal(&decoded, &saved_decoded),
	      "INFO0a decoding mutated output after invalid input");
	payload[2] = 1;
	CHECK(v90_info0a_decode(
		      payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded) == V90_INFO0_INVALID_PAYLOAD,
	      "INFO0a accepted nonzero packed padding");
	payload[2] = 0;
	payload[1] = UINT8_C(0xb4);
	CHECK(v90_info0a_decode(
		      payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded) == V90_INFO0_INVALID_PAYLOAD &&
	      v90_info0a_equal(&decoded, &saved_decoded) &&
	      !v90_info0_is_valid(NULL) && !v90_info0d_is_valid(NULL) &&
	      strcmp(v90_info0_result_name(V90_INFO0_INVALID_PAYLOAD),
		     "invalid payload") == 0 &&
	      strcmp(v90_info0_result_name(
		     (enum v90_info0_result)4), "unknown") == 0,
	      "INFO0 accepted a reserved field or result");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire images", test_wire_images },
		{ "reserved bits", test_reserved_bits },
		{ "field comparison", test_comparison },
		{ "shared V.34 capabilities", test_shared_v34_capabilities },
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
