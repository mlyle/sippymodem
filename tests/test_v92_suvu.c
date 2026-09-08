// SPDX-License-Identifier: BSD-3-Clause

#include <v92_suvu.h>
#include <v92_status_update.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	SUVU_WAIT_FOR_CPU_BIT = 26,
	SUVU_LEVEL_FIRST_BIT = 27,
	SUVU_REQUEST_SILENCE_BIT = 32,
	SUVU_ACKNOWLEDGMENT_BIT = 33,
	SUVU_CRC_START_BIT = 34,
	SUVU_CRC_FIRST_BIT = 35,
	SUVU_CRC_BITS = 16,
	SUVU_FILL_FIRST_BIT = 51,
};

static uint8_t bit_at(const uint8_t *frame, size_t bit)
{
	return (uint8_t)((frame[bit / 8u] >> (7u - bit % 8u)) & 1u);
}

static void set_bit(uint8_t *frame, size_t bit, uint8_t value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit % 8u));

	if (value != 0)
		frame[bit / 8u] |= mask;
	else
		frame[bit / 8u] &= (uint8_t)~mask;
}

static uint32_t lsb_field(
	const uint8_t *frame, size_t first, size_t count)
{
	uint32_t value = 0;

	for (size_t bit = 0; bit < count; ++bit)
		value |= (uint32_t)bit_at(frame, first + bit) << bit;
	return value;
}

static uint16_t msb_field(
	const uint8_t *frame, size_t first, size_t count)
{
	uint16_t value = 0;

	for (size_t bit = 0; bit < count; ++bit)
		value = (uint16_t)((value << 1) | bit_at(frame, first + bit));
	return value;
}

static bool bits_are(
	const uint8_t *frame, size_t first, size_t count, uint8_t value)
{
	for (size_t bit = 0; bit < count; ++bit) {
		if (bit_at(frame, first + bit) != value)
			return false;
	}
	return true;
}

static uint16_t reference_crc(const uint8_t *frame)
{
	uint16_t remainder = UINT16_C(0xffff);

	for (size_t bit = 18; bit < SUVU_CRC_START_BIT; ++bit) {
		uint16_t feedback = (uint16_t)(
			((remainder >> 15) ^ bit_at(frame, bit)) & 1u);

		remainder = (uint16_t)(remainder << 1);
		if (feedback != 0)
			remainder ^= UINT16_C(0x1021);
	}
	return remainder;
}

static void rewrite_crc(uint8_t *frame)
{
	uint16_t crc = reference_crc(frame);

	for (size_t bit = 0; bit < SUVU_CRC_BITS; ++bit) {
		set_bit(
			frame, SUVU_CRC_FIRST_BIT + bit,
			(uint8_t)((crc >> (SUVU_CRC_BITS - bit - 1u)) & 1u));
	}
}

static bool parameters_match(
	const struct v92_suvu_parameters *left,
	const struct v92_suvu_parameters *right)
{
	return left->prefilter_level_db_q2_2 ==
		       right->prefilter_level_db_q2_2 &&
	       left->wait_for_cpu == right->wait_for_cpu &&
	       left->request_silence == right->request_silence &&
	       left->acknowledges_cpd == right->acknowledges_cpd;
}

static bool test_dimensions(void)
{
	CHECK(V92_SUVU_FRAME_BITS == 72 && V92_SUVU_FRAME_BYTES == 9 &&
	      v92_suvu_frame_bits(V92_CONTROL_UPSTREAM_4_POINT_BITS) == 72 &&
	      v92_suvu_frame_bits(V92_CONTROL_UPSTREAM_8_POINT_BITS) == 72 &&
	      v92_suvu_frame_bytes(V92_CONTROL_UPSTREAM_4_POINT_BITS) == 9 &&
	      v92_suvu_frame_bytes(V92_CONTROL_UPSTREAM_8_POINT_BITS) == 9 &&
	      v92_suvu_frame_bits(6u) == 0 &&
	      v92_suvu_frame_bytes(6u) == 0 &&
	      v92_status_update_frame_bits(
		      V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS) == 54 &&
	      v92_status_update_frame_bytes(
		      V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS) == 7 &&
	      v92_status_update_frame_bits(
		      V92_CONTROL_UPSTREAM_4_POINT_BITS) == 72 &&
	      v92_status_update_frame_bits(
		      V92_CONTROL_UPSTREAM_8_POINT_BITS) == 72 &&
	      v92_status_update_frame_bits(0u) == 0u,
	      "SUVu training dimensions changed");
	return true;
}

static bool test_shared_framing(void)
{
	uint8_t frame[V92_STATUS_UPDATE_MAX_FRAME_BYTES + 1u];
	uint16_t decoded = 0u;
	size_t frame_bits = 0u;
	const uint16_t payload = UINT16_C(0x5a5a);

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v92_status_update_encode(
		      payload, V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS,
		      frame, 7u, &frame_bits) == V92_STATUS_UPDATE_OK &&
	      frame_bits == 54u && frame[7] == 0xa5 &&
	      bit_at(frame, 54u) == 0u && bit_at(frame, 55u) == 0u &&
	      v92_status_update_decode(
		      frame, 7u, frame_bits,
		      V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS, &decoded) ==
		      V92_STATUS_UPDATE_OK &&
	      decoded == payload,
	      "shared status-update framing did not round-trip");
	return true;
}

static bool test_wire_fields(void)
{
	struct v92_suvu_parameters parameters = {
		.prefilter_level_db_q2_2 = -11,
		.wait_for_cpu = true,
		.request_silence = true,
		.acknowledges_cpd = true,
	};
	uint8_t frame[V92_SUVU_FRAME_BYTES + 1];
	size_t frame_bits = 0;

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v92_suvu_encode(
		      &parameters, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      frame, V92_SUVU_FRAME_BYTES, &frame_bits) == V92_SUVU_OK,
	      "SUVu wire fixture did not encode");
	CHECK(frame_bits == V92_SUVU_FRAME_BITS &&
	      frame[V92_SUVU_FRAME_BYTES] == 0xa5 &&
	      bits_are(frame, 0, 17, 1) && bit_at(frame, 17) == 0 &&
	      bit_at(frame, 18) == 1 && bits_are(frame, 19, 7, 0) &&
	      bit_at(frame, SUVU_WAIT_FOR_CPU_BIT) == 1 &&
	      lsb_field(frame, SUVU_LEVEL_FIRST_BIT, 5) == 21 &&
	      bit_at(frame, SUVU_REQUEST_SILENCE_BIT) == 1 &&
	      bit_at(frame, SUVU_ACKNOWLEDGMENT_BIT) == 1 &&
	      bit_at(frame, SUVU_CRC_START_BIT) == 0,
	      "SUVu fixed or information fields changed");
	CHECK(reference_crc(frame) == UINT16_C(0xbd0d) &&
	      msb_field(frame, SUVU_CRC_FIRST_BIT, SUVU_CRC_BITS) ==
		      UINT16_C(0xbd0d) &&
	      bits_are(
		      frame, SUVU_FILL_FIRST_BIT,
		      V92_SUVU_FRAME_BITS - SUVU_FILL_FIRST_BIT, 0),
	      "SUVu CRC or fill changed");
	return true;
}

static bool test_all_values_round_trip(void)
{
	for (int level = -16; level <= 15; ++level) {
		for (unsigned flags = 0; flags < 8; ++flags) {
			struct v92_suvu_parameters wanted = {
				.prefilter_level_db_q2_2 = (int8_t)level,
				.wait_for_cpu = (flags & 1u) != 0,
				.request_silence = (flags & 2u) != 0,
				.acknowledges_cpd = (flags & 4u) != 0,
			};
			struct v92_suvu_parameters decoded;
			uint8_t frame[V92_SUVU_FRAME_BYTES];
			size_t frame_bits = 0;
			unsigned int alignment =
				(flags & 1u) != 0 ? V92_CONTROL_UPSTREAM_8_POINT_BITS :
							 V92_CONTROL_UPSTREAM_4_POINT_BITS;

			memset(&decoded, 0xa5, sizeof(decoded));
			CHECK(v92_suvu_parameters_are_valid(&wanted) &&
			      v92_suvu_encode(
				      &wanted, alignment, frame, sizeof(frame),
				      &frame_bits) == V92_SUVU_OK &&
			      v92_suvu_decode(
				      frame, sizeof(frame), frame_bits,
				      alignment, &decoded) == V92_SUVU_OK &&
			      parameters_match(&wanted, &decoded),
			      "SUVu level %d flags 0x%x did not round-trip",
			      level, flags);
		}
	}
	return true;
}

static bool test_data_widths(void)
{
	for (unsigned int width = 0u; width <= 73u; ++width) {
		bool valid = width == 24u ||
			(width >= 36u && width <= 72u && width % 2u == 0u);
		size_t expected = valid ? (52u + width - 1u) / width * width : 0u;
		CHECK(v92_suvu_frame_bits(width) == expected,
		      "SUVu width %u has the wrong extent", width);
		if (!valid)
			continue;
		for (unsigned int flags = 0u; flags < 8u; ++flags) {
			struct v92_suvu_parameters wanted = {
				.prefilter_level_db_q2_2 = (int8_t)((int)width % 32 - 16),
				.wait_for_cpu = (flags & 1u) != 0u,
				.request_silence = (flags & 2u) != 0u,
				.acknowledges_cpd = (flags & 4u) != 0u,
			};
			struct v92_suvu_parameters decoded;
			uint8_t frame[V92_SUVU_MAX_FRAME_BYTES + 1u];
			size_t bits = 0u;
			size_t bytes = (expected + 7u) / 8u;

			memset(frame, 0xa5, sizeof(frame));
			CHECK(v92_suvu_encode(&wanted, width, frame, bytes - 1u,
				      &bits) == V92_SUVU_OUTPUT_TOO_SMALL &&
			      bits == 0u && frame[0] == 0xa5 && frame[bytes] == 0xa5,
			      "short SUVu width %u output was modified", width);
			CHECK(v92_suvu_encode(&wanted, width, frame, bytes, &bits) ==
				      V92_SUVU_OK && bits == expected &&
			      frame[bytes] == 0xa5 &&
			      v92_suvu_decode(frame, bytes, bits, width, &decoded) ==
				      V92_SUVU_OK && parameters_match(&wanted, &decoded),
			      "SUVu width %u flags %u failed", width, flags);
		}
	}
	return true;
}

static bool decode_rejects_unchanged(
	const uint8_t *frame, size_t capacity, size_t frame_bits,
	unsigned int alignment,
	enum v92_suvu_result wanted)
{
	struct v92_suvu_parameters decoded;
	struct v92_suvu_parameters before;

	memset(&decoded, 0xa5, sizeof(decoded));
	before = decoded;
	return v92_suvu_decode(
		       frame, capacity, frame_bits, alignment, &decoded) ==
		       wanted &&
	       memcmp(&decoded, &before, sizeof(decoded)) == 0;
}

static bool test_decode_corruption(void)
{
	struct v92_suvu_parameters parameters = {
		.prefilter_level_db_q2_2 = V92_SUVU_NO_MEASUREMENT_Q2_2,
		.wait_for_cpu = true,
	};
	struct v92_suvu_parameters decoded;
	uint8_t original[V92_SUVU_FRAME_BYTES];
	uint8_t changed[V92_SUVU_FRAME_BYTES];
	size_t frame_bits = 0;

	CHECK(v92_suvu_encode(
		      &parameters, V92_CONTROL_UPSTREAM_8_POINT_BITS,
		      original, sizeof(original), &frame_bits) == V92_SUVU_OK,
	      "SUVu corruption fixture did not encode");

	memcpy(changed, original, sizeof(changed));
	set_bit(changed, 0, 0);
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_INVALID_FRAME),
	      "SUVu accepted a broken sync prefix");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, 17, 1);
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_INVALID_FRAME),
	      "SUVu accepted a nonzero initial start bit");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, 18, 0);
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_INVALID_FRAME),
	      "SUVu accepted another record type");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, SUVU_CRC_START_BIT, 1);
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_INVALID_FRAME),
	      "SUVu accepted a nonzero CRC start bit");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, SUVU_WAIT_FOR_CPU_BIT,
		(uint8_t)!bit_at(changed, SUVU_WAIT_FOR_CPU_BIT));
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_CRC_MISMATCH),
	      "SUVu accepted corrupted information");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, SUVU_CRC_FIRST_BIT,
		(uint8_t)!bit_at(changed, SUVU_CRC_FIRST_BIT));
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_CRC_MISMATCH),
	      "SUVu accepted a corrupted CRC");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, SUVU_FILL_FIRST_BIT, 1);
	CHECK(decode_rejects_unchanged(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, V92_SUVU_INVALID_FRAME),
	      "SUVu accepted nonzero fill");

	memcpy(changed, original, sizeof(changed));
	for (size_t bit = 19; bit <= 25; ++bit)
		set_bit(changed, bit, 1);
	rewrite_crc(changed);
	CHECK(v92_suvu_decode(
		      changed, sizeof(changed), frame_bits,
		      V92_CONTROL_UPSTREAM_8_POINT_BITS, &decoded) == V92_SUVU_OK &&
	      parameters_match(&parameters, &decoded),
	      "SUVu decoder interpreted reserved bits");
	return true;
}

static bool test_bounds_and_transactionality(void)
{
	struct v92_suvu_parameters valid = {
		.prefilter_level_db_q2_2 = 15,
	};
	struct v92_suvu_parameters invalid = valid;
	uint8_t frame[V92_SUVU_FRAME_BYTES];
	uint8_t before[V92_SUVU_FRAME_BYTES];
	size_t frame_bits = 1234;

	invalid.prefilter_level_db_q2_2 = -17;
	memset(frame, 0xa5, sizeof(frame));
	memcpy(before, frame, sizeof(before));
	CHECK(!v92_suvu_parameters_are_valid(NULL) &&
	      !v92_suvu_parameters_are_valid(&invalid) &&
	      v92_suvu_encode(
		      &invalid, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      frame, sizeof(frame), &frame_bits) ==
		      V92_SUVU_INVALID_PARAMETERS &&
	      frame_bits == 1234 &&
	      memcmp(frame, before, sizeof(frame)) == 0,
	      "invalid SUVu parameters changed outputs");
	invalid.prefilter_level_db_q2_2 = 16;
	CHECK(!v92_suvu_parameters_are_valid(&invalid),
	      "SUVu accepted a level above signed Q2.2");
	CHECK(v92_suvu_encode(
		      &valid, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      frame, sizeof(frame) - 1u, &frame_bits) ==
		      V92_SUVU_OUTPUT_TOO_SMALL &&
	      frame_bits == 1234 &&
	      memcmp(frame, before, sizeof(frame)) == 0,
	      "short SUVu output changed outputs");
	CHECK(v92_suvu_encode(
		      &valid, 6u,
		      frame, sizeof(frame), &frame_bits) ==
		      V92_SUVU_INVALID_PARAMETERS &&
	      v92_suvu_encode(
		      NULL, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      frame, sizeof(frame), &frame_bits) ==
		      V92_SUVU_INVALID_ARGUMENT &&
	      v92_suvu_encode(
		      &valid, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      NULL, sizeof(frame), &frame_bits) ==
		      V92_SUVU_INVALID_ARGUMENT &&
	      v92_suvu_encode(
		      &valid, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      frame, sizeof(frame), NULL) ==
		      V92_SUVU_INVALID_ARGUMENT,
	      "SUVu encoder accepted invalid arguments");

	CHECK(v92_suvu_encode(
		      &valid, V92_CONTROL_UPSTREAM_4_POINT_BITS,
		      frame, sizeof(frame), &frame_bits) == V92_SUVU_OK,
	      "SUVu bounds fixture did not encode");
	CHECK(decode_rejects_unchanged(
		      frame, sizeof(frame) - 1u, frame_bits,
		      V92_CONTROL_UPSTREAM_4_POINT_BITS, V92_SUVU_INVALID_ARGUMENT) &&
	      decode_rejects_unchanged(
		      frame, sizeof(frame), frame_bits - 1u,
		      V92_CONTROL_UPSTREAM_4_POINT_BITS, V92_SUVU_INVALID_FRAME) &&
	      decode_rejects_unchanged(
		      frame, sizeof(frame), frame_bits,
		      6u, V92_SUVU_INVALID_FRAME) &&
	      v92_suvu_decode(
		      NULL, sizeof(frame), frame_bits,
		      V92_CONTROL_UPSTREAM_4_POINT_BITS, &valid) ==
		      V92_SUVU_INVALID_ARGUMENT &&
	      v92_suvu_decode(
		      frame, sizeof(frame), frame_bits,
		      V92_CONTROL_UPSTREAM_4_POINT_BITS, NULL) ==
		      V92_SUVU_INVALID_ARGUMENT,
	      "SUVu decoder accepted invalid bounds or arguments");
	return true;
}

static bool test_result_names(void)
{
	CHECK(strcmp(v92_suvu_result_name(V92_SUVU_OK), "ok") == 0 &&
	      strcmp(v92_suvu_result_name(V92_SUVU_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v92_suvu_result_name(V92_SUVU_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v92_suvu_result_name(V92_SUVU_OUTPUT_TOO_SMALL),
		     "output too small") == 0 &&
	      strcmp(v92_suvu_result_name(V92_SUVU_INVALID_FRAME),
		     "invalid frame") == 0 &&
	      strcmp(v92_suvu_result_name(V92_SUVU_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(v92_suvu_result_name((enum v92_suvu_result)99),
		     "unknown") == 0,
	      "SUVu result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "dimensions", test_dimensions },
		{ "shared framing", test_shared_framing },
		{ "wire fields", test_wire_fields },
		{ "all values round trip", test_all_values_round_trip },
		{ "data widths", test_data_widths },
		{ "decode corruption", test_decode_corruption },
		{ "bounds and transactionality", test_bounds_and_transactionality },
		{ "result names", test_result_names },
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
