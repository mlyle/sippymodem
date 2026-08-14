// SPDX-License-Identifier: BSD-3-Clause

#include <v92_suvd.h>

#include "test.h"

#include <string.h>

enum {
	SUVD_REQUEST_SILENCE_BIT = 32,
	SUVD_ACKNOWLEDGMENT_BIT = 33,
	SUVD_CRC_START_BIT = 34,
	SUVD_CRC_FIRST_BIT = 35,
	SUVD_CRC_BITS = 16,
	SUVD_FILL_FIRST_BIT = 51,
	FIXTURE_FRAME_BITS = 9,
	CORRUPTION_FRAME_BITS = 17,
};

static uint8_t bit_at(const uint8_t *frame, size_t bit)
{
	return (uint8_t)(
		(frame[bit / 8u] >> (7u - bit % 8u)) & 1u);
}

static void set_bit(uint8_t *frame, size_t bit, uint8_t value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit % 8u));

	if (value != 0u)
		frame[bit / 8u] |= mask;
	else
		frame[bit / 8u] &= (uint8_t)~mask;
}

static bool bits_are(
	const uint8_t *frame, size_t first_bit,
	size_t bit_count, uint8_t value)
{
	for (size_t bit = 0u; bit < bit_count; ++bit) {
		if (bit_at(frame, first_bit + bit) != value)
			return false;
	}
	return true;
}

static uint16_t msb_field(
	const uint8_t *frame, size_t first_bit, size_t bit_count)
{
	uint16_t value = 0u;

	for (size_t bit = 0u; bit < bit_count; ++bit)
		value = (uint16_t)((value << 1) |
			bit_at(frame, first_bit + bit));
	return value;
}

static uint16_t reference_crc(const uint8_t *frame)
{
	uint16_t remainder = UINT16_C(0xffff);

	for (size_t bit = 18u; bit < SUVD_CRC_START_BIT; ++bit) {
		uint16_t feedback = (uint16_t)(
			((remainder >> 15) ^ bit_at(frame, bit)) & 1u);

		remainder = (uint16_t)(remainder << 1);
		if (feedback != 0u)
			remainder ^= UINT16_C(0x1021);
	}
	return remainder;
}

static void rewrite_crc(uint8_t *frame)
{
	uint16_t crc = reference_crc(frame);

	for (size_t bit = 0u; bit < SUVD_CRC_BITS; ++bit) {
		set_bit(
			frame, SUVD_CRC_FIRST_BIT + bit,
			(uint8_t)((crc >> (SUVD_CRC_BITS - bit - 1u)) & 1u));
	}
}

static bool parameters_match(
	const struct v92_suvd_parameters *left,
	const struct v92_suvd_parameters *right)
{
	return left->request_silence == right->request_silence &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static bool test_wire_fields(void)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = true,
		.acknowledges_cpu = true,
	};
	uint8_t frame[V92_SUVD_MAX_FRAME_BYTES + 1u];
	size_t frame_bits = 0u;
	size_t frame_bytes = v92_suvd_frame_bytes(FIXTURE_FRAME_BITS);

	CHECK(V92_SUVD_MAX_FRAME_BITS == 75 &&
	      V92_SUVD_MAX_FRAME_BYTES == 10 && frame_bytes == 7u &&
	      v92_suvd_frame_bits(FIXTURE_FRAME_BITS) == 54u,
	      "SUVd dimensions changed");
	memset(frame, 0xa5, sizeof(frame));
	CHECK(v92_suvd_encode(
		      &parameters, FIXTURE_FRAME_BITS, frame, frame_bytes,
		      &frame_bits) == V92_SUVD_OK,
	      "SUVd wire fixture did not encode");
	CHECK(frame_bits == 54u && frame[frame_bytes] == 0xa5 &&
	      bits_are(frame, 0u, 17u, 1u) && bit_at(frame, 17u) == 0u &&
	      bit_at(frame, 18u) == 1u && bits_are(frame, 19u, 13u, 0u) &&
	      bit_at(frame, SUVD_REQUEST_SILENCE_BIT) == 1u &&
	      bit_at(frame, SUVD_ACKNOWLEDGMENT_BIT) == 1u &&
	      bit_at(frame, SUVD_CRC_START_BIT) == 0u,
	      "SUVd fixed or information fields changed");
	CHECK(reference_crc(frame) == UINT16_C(0x36f4) &&
	      msb_field(frame, SUVD_CRC_FIRST_BIT, SUVD_CRC_BITS) ==
		      UINT16_C(0x36f4) &&
	      bits_are(
		      frame, SUVD_FILL_FIRST_BIT,
		      frame_bits - SUVD_FILL_FIRST_BIT, 0u) &&
	      bits_are(
		      frame, frame_bits,
		      frame_bytes * 8u - frame_bits, 0u),
	      "SUVd CRC, fill, or storage padding changed");
	return true;
}

static bool test_round_trip(void)
{
	for (uint8_t width = V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS;
	     width <= V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MAX_BITS; ++width) {
		for (unsigned int flags = 0u; flags < 4u; ++flags) {
			struct v92_suvd_parameters wanted = {
				.request_silence = (flags & 1u) != 0u,
				.acknowledges_cpu = (flags & 2u) != 0u,
			};
			struct v92_suvd_parameters decoded;
			uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
			size_t expected_bits =
				(V92_SUVD_INFORMATION_BITS + V92_SUVD_TRAILER_BITS +
				 width - 1u) / width * width;
			size_t frame_bits = 0u;

			memset(&decoded, 0xa5, sizeof(decoded));
			CHECK(v92_suvd_parameters_are_valid(&wanted) &&
			      v92_suvd_encode(
				      &wanted, width, frame, sizeof(frame),
				      &frame_bits) == V92_SUVD_OK &&
			      frame_bits == expected_bits &&
			      v92_suvd_frame_bits(width) == expected_bits &&
			      v92_suvd_frame_bytes(width) ==
				      (expected_bits + 7u) / 8u &&
			      v92_suvd_decode(
				      frame, sizeof(frame), frame_bits, width,
				      &decoded) == V92_SUVD_OK &&
			      parameters_match(&wanted, &decoded),
			      "SUVd flags 0x%x width %u did not round-trip",
			      flags, width);
		}
	}
	return true;
}

static bool decode_rejects_unchanged(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bits, uint8_t training_frame_bits,
	enum v92_suvd_result expected)
{
	struct v92_suvd_parameters decoded;
	uint8_t unchanged[sizeof(decoded)];

	memset(&decoded, 0xa5, sizeof(decoded));
	memcpy(unchanged, &decoded, sizeof(decoded));
	return v92_suvd_decode(
		       frame, frame_capacity, frame_bits,
		       training_frame_bits, &decoded) == expected &&
	       memcmp(&decoded, unchanged, sizeof(decoded)) == 0;
}

static bool test_corruption(void)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = true,
	};
	struct v92_suvd_parameters decoded;
	uint8_t original[V92_SUVD_MAX_FRAME_BYTES] = { 0 };
	uint8_t changed[V92_SUVD_MAX_FRAME_BYTES];
	size_t frame_bits = 0u;
	static const size_t fixed_bits[] = {
		0u, 17u, 18u, SUVD_CRC_START_BIT, SUVD_FILL_FIRST_BIT, 68u,
	};

	CHECK(v92_suvd_encode(
		      &parameters, CORRUPTION_FRAME_BITS,
		      original, sizeof(original), &frame_bits) == V92_SUVD_OK &&
	      frame_bits == 68u,
	      "SUVd corruption fixture did not encode");
	for (size_t index = 0u; index < ARRAY_LENGTH(fixed_bits); ++index) {
		memcpy(changed, original, sizeof(changed));
		set_bit(
			changed, fixed_bits[index],
			(uint8_t)!bit_at(changed, fixed_bits[index]));
		CHECK(decode_rejects_unchanged(
			      changed, sizeof(changed), frame_bits,
			      CORRUPTION_FRAME_BITS,
			      V92_SUVD_INVALID_FRAME),
		      "SUVd accepted broken fixed bit %zu",
		      fixed_bits[index]);
	}
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, 19u, 1u);
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      CORRUPTION_FRAME_BITS,
		      V92_SUVD_CRC_MISMATCH),
	      "SUVd missed a covered reserved-bit error");
	rewrite_crc(changed);
	CHECK(v92_suvd_decode(
		      changed, sizeof(changed), frame_bits,
		      CORRUPTION_FRAME_BITS, &decoded) ==
		      V92_SUVD_OK && parameters_match(&decoded, &parameters),
	      "SUVd did not ignore a valid covered reserved bit");
	return true;
}

static bool test_bounds_and_names(void)
{
	const struct v92_suvd_parameters parameters = { 0 };
	struct v92_suvd_parameters output;
	uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t unchanged[sizeof(frame)];
	size_t frame_bits = 123u;
	const uint8_t width = 30u;
	const size_t frame_bytes = v92_suvd_frame_bytes(width);
	static const char *const names[] = {
		"ok", "invalid argument", "output too small",
		"invalid frame", "CRC mismatch",
	};

	memset(frame, 0xa5, sizeof(frame));
	memcpy(unchanged, frame, sizeof(frame));
	CHECK(!v92_suvd_parameters_are_valid(NULL) &&
	      v92_suvd_encode(
		      NULL, width, frame, sizeof(frame), &frame_bits) ==
		      V92_SUVD_INVALID_ARGUMENT &&
	      v92_suvd_encode(
		      &parameters, width, NULL, sizeof(frame), &frame_bits) ==
		      V92_SUVD_INVALID_ARGUMENT &&
	      v92_suvd_encode(
		      &parameters, width, frame, sizeof(frame), NULL) ==
		      V92_SUVD_INVALID_ARGUMENT &&
	      v92_suvd_encode(
		      &parameters, width, frame, frame_bytes - 1u,
		      &frame_bits) ==
		      V92_SUVD_OUTPUT_TOO_SMALL &&
	      v92_suvd_encode(
		      &parameters, 8u, frame, sizeof(frame), &frame_bits) ==
		      V92_SUVD_INVALID_ARGUMENT &&
	      v92_suvd_frame_bits(8u) == 0u &&
	      v92_suvd_frame_bytes(31u) == 0u &&
	      frame_bits == 123u &&
	      memcmp(frame, unchanged, sizeof(frame)) == 0,
	      "invalid SUVd encoding changed output");
	CHECK(v92_suvd_encode(
		      &parameters, width, frame, sizeof(frame), &frame_bits) ==
		      V92_SUVD_OK &&
	      decode_rejects_unchanged(
		      frame, frame_bytes - 1u, frame_bits, width,
		      V92_SUVD_INVALID_ARGUMENT) &&
	      decode_rejects_unchanged(
		      frame, sizeof(frame), frame_bits - 1u, width,
		      V92_SUVD_INVALID_FRAME) &&
	      decode_rejects_unchanged(
		      frame, sizeof(frame), frame_bits, 8u,
		      V92_SUVD_INVALID_FRAME) &&
	      v92_suvd_decode(
		      NULL, sizeof(frame), frame_bits, width, &output) ==
		      V92_SUVD_INVALID_ARGUMENT &&
	      v92_suvd_decode(
		      frame, sizeof(frame), frame_bits, width, NULL) ==
		      V92_SUVD_INVALID_ARGUMENT,
	      "SUVd decoder accepted invalid bounds");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index) {
		CHECK(strcmp(
			      v92_suvd_result_name(
				      (enum v92_suvd_result)index),
			      names[index]) == 0,
		      "SUVd result name %zu changed", index);
	}
	CHECK(strcmp(
		      v92_suvd_result_name((enum v92_suvd_result)99),
		      "unknown") == 0,
	      "unknown SUVd result name changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire fields", test_wire_fields },
		{ "round trip", test_round_trip },
		{ "corruption", test_corruption },
		{ "bounds and names", test_bounds_and_names },
	};
	unsigned int failures = 0u;

	for (size_t index = 0u; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0u ? 0 : 1;
}
