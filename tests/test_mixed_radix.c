// SPDX-License-Identifier: BSD-3-Clause

#include <mixed_radix.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition, ...)                                                \
	do {                                                                   \
		if (!(condition)) {                                               \
			fprintf(stderr, __VA_ARGS__);                               \
			fputc('\n', stderr);                                       \
			return 1;                                                  \
		}                                                              \
	} while (0)

static void bits_from_u64(uint8_t *bits, size_t count, uint64_t value)
{
	for (size_t bit = 0; bit < count; ++bit)
		bits[bit] = (uint8_t)((value >> bit) & 1u);
}

static int test_uneven_round_trip(void)
{
	static const uint32_t radices[] = { 3, 5, 7, 11, 13, 17 };
	uint8_t input[17];
	uint8_t output[17] = { 0 };
	uint32_t digits[6] = { 0 };
	uint64_t value = UINT64_C(0x15a3d);
	uint64_t remaining = value;

	bits_from_u64(input, sizeof(input), value);
	CHECK(mixed_radix_encode(
		      radices, 6, input, sizeof(input), digits) ==
		      MIXED_RADIX_OK,
	      "uneven encode failed");
	for (size_t digit = 0; digit < 6; ++digit) {
		CHECK(digits[digit] == remaining % radices[digit],
		      "uneven digit %zu changed", digit);
		remaining /= radices[digit];
	}
	CHECK(remaining == 0,
	      "uneven fixture exceeded its mixed-radix capacity");
	CHECK(mixed_radix_decode(
		      radices, 6, digits, output, sizeof(output)) ==
		      MIXED_RADIX_OK &&
	      memcmp(input, output, sizeof(input)) == 0,
	      "uneven round trip changed bits");
	return 0;
}

static int test_wide_round_trip(void)
{
	uint32_t radices[12];
	uint8_t input[72];
	uint8_t output[72] = { 0 };
	uint32_t digits[12] = { 0 };

	for (size_t digit = 0; digit < 12; ++digit)
		radices[digit] = 64;
	for (size_t bit = 0; bit < sizeof(input); ++bit)
		input[bit] = (uint8_t)(((bit * 29u + bit / 3u) >> 1u) & 1u);
	CHECK(mixed_radix_encode(
		      radices, 12, input, sizeof(input), digits) ==
		      MIXED_RADIX_OK,
	      "72-bit encode failed");
	CHECK(mixed_radix_decode(
		      radices, 12, digits, output, sizeof(output)) ==
		      MIXED_RADIX_OK &&
	      memcmp(input, output, sizeof(input)) == 0,
	      "72-bit round trip changed bits");
	return 0;
}

static int test_differential_round_trip(void)
{
	static const uint32_t radices[] = { 2, 2, 2 };
	static const uint8_t values[] = { 7, 0, 7, 3, 6, 1 };
	uint8_t transmit_state = 0;
	uint8_t receive_state = 0;

	for (size_t frame = 0; frame < sizeof(values); ++frame) {
		uint8_t input[3];
		uint8_t output[3] = { 0 };
		uint32_t digits[3] = { 9, 9, 9 };
		uint8_t sign = 9;

		bits_from_u64(input, sizeof(input), values[frame]);
		CHECK(mixed_radix_encode_differential(
			      radices, 3, input, sizeof(input),
			      &transmit_state, digits, &sign) ==
			      MIXED_RADIX_OK,
		      "differential encode failed at frame %zu", frame);
		CHECK(sign == transmit_state,
		      "frame sign and transmit state diverged");
		CHECK(mixed_radix_decode_differential(
			      radices, 3, digits, &receive_state,
			      output, sizeof(output)) == MIXED_RADIX_OK,
		      "differential decode failed at frame %zu", frame);
		CHECK(memcmp(input, output, sizeof(input)) == 0 &&
			      receive_state == transmit_state,
		      "differential frame %zu changed state or bits", frame);
	}
	return 0;
}

static int test_transactionality(void)
{
	static const uint32_t good_radices[] = { 2, 2, 2 };
	static const uint32_t zero_radix[] = { 2, 0, 2 };
	static const uint32_t small_capacity[] = { 2, 2 };
	uint8_t invalid_bits[3] = { 0, 2, 1 };
	uint8_t bits[3] = { 1, 1, 1 };
	uint8_t output[3] = { 7, 7, 7 };
	uint32_t digits[3] = { 11, 12, 13 };
	uint32_t invalid_digits[3] = { 0, 2, 0 };
	uint8_t state = 0;
	uint8_t sign = 7;

	CHECK(mixed_radix_encode(
		      zero_radix, 3, bits, 3, digits) ==
		      MIXED_RADIX_INVALID_CONFIGURATION &&
	      digits[0] == 11 && digits[1] == 12 && digits[2] == 13,
	      "zero radix changed encode output");
	CHECK(mixed_radix_encode(
		      small_capacity, 2, bits, 3, digits) ==
		      MIXED_RADIX_INVALID_CONFIGURATION &&
	      digits[0] == 11,
	      "insufficient capacity changed encode output");
	CHECK(mixed_radix_encode(
		      good_radices, 3, invalid_bits, 3, digits) ==
		      MIXED_RADIX_OUT_OF_RANGE &&
	      digits[0] == 11,
	      "non-binary input changed encode output");
	CHECK(mixed_radix_decode(
		      good_radices, 3, invalid_digits, output, 3) ==
		      MIXED_RADIX_OUT_OF_RANGE &&
	      output[0] == 7 && output[1] == 7 && output[2] == 7,
	      "invalid tuple changed decoded bits");
	CHECK(mixed_radix_encode_differential(
		      good_radices, 3, invalid_bits, 3,
		      &state, digits, &sign) == MIXED_RADIX_OUT_OF_RANGE &&
	      state == 0 && sign == 7 && digits[0] == 11,
	      "rejected differential encode changed state");
	state = 2;
	CHECK(mixed_radix_decode_differential(
		      good_radices, 3, invalid_digits, &state,
		      output, 3) == MIXED_RADIX_OUT_OF_RANGE &&
	      state == 2 && output[0] == 7,
	      "rejected differential decode changed state");
	CHECK(mixed_radix_encode(
		      good_radices, 3, bits, 3, NULL) ==
		      MIXED_RADIX_INVALID_ARGUMENT &&
	      mixed_radix_decode(
		      good_radices, 3, NULL, output, 3) ==
		      MIXED_RADIX_INVALID_ARGUMENT,
	      "invalid pointer result changed");
	return 0;
}

static int test_result_names(void)
{
	CHECK(strcmp(mixed_radix_result_name(MIXED_RADIX_OK), "ok") == 0 &&
	      strcmp(mixed_radix_result_name(
		     MIXED_RADIX_INVALID_CONFIGURATION),
		     "invalid configuration") == 0 &&
	      strcmp(mixed_radix_result_name(
		     (enum mixed_radix_result)99), "unknown") == 0,
	      "mixed-radix result names changed");
	return 0;
}

int main(void)
{
	CHECK(test_uneven_round_trip() == 0, "uneven round trip failed");
	CHECK(test_wide_round_trip() == 0, "wide round trip failed");
	CHECK(test_differential_round_trip() == 0,
	      "differential round trip failed");
	CHECK(test_transactionality() == 0, "transactionality failed");
	CHECK(test_result_names() == 0, "result names failed");
	return 0;
}
