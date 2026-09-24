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

static int test_received_low_bits(void)
{
	const uint32_t radices[] = { 5, 5 };
	uint8_t bits[5];
	uint8_t saved[5];
	uint32_t digits[2];

	/* The 4-bit encoder uses only 16 of the 25 possible decisions. Noise
	 * can select any tuple; strict inversion must still reject the other 9. */
	for (uint32_t value = 0; value < 25u; ++value) {
		digits[0] = value % 5u;
		digits[1] = value / 5u;
		memset(bits, 0xa5, sizeof(bits));
		memcpy(saved, bits, sizeof(bits));
		CHECK(mixed_radix_decode(radices, 2, digits, bits, 4) ==
			      (value < 16u ? MIXED_RADIX_OK : MIXED_RADIX_OUT_OF_RANGE),
		      "strict inverse accepted unused tuple %u", value);
		if (value >= 16u)
			CHECK(memcmp(bits, saved, sizeof(bits)) == 0,
			      "strict inverse changed unused tuple output");
		CHECK(mixed_radix_decode_low_bits(radices, 2, digits, bits, 4) ==
			      MIXED_RADIX_OK && bits[4] == 0xa5,
		      "received tuple %u failed or wrote beyond its bits", value);
		for (size_t bit = 0; bit < 4; ++bit)
			CHECK(bits[bit] == ((value >> bit) & 1u),
			      "received tuple %u bit %zu changed", value, bit);
	}
	memcpy(saved, bits, sizeof(bits));
	digits[0] = 5;
	CHECK(mixed_radix_decode_low_bits(radices, 2, digits, bits, 4) ==
		      MIXED_RADIX_OUT_OF_RANGE &&
	      mixed_radix_decode_low_bits(radices, 2, digits, bits, 5) ==
		      MIXED_RADIX_INVALID_CONFIGURATION &&
	      mixed_radix_decode_low_bits(radices, 2, NULL, bits, 4) ==
		      MIXED_RADIX_INVALID_ARGUMENT &&
	      memcmp(bits, saved, sizeof(bits)) == 0,
	      "invalid received tuple changed its output");
	return 0;
}

static int test_transactionality(void)
{
	static const uint32_t good_radices[] = { 2, 2, 2 };
	static const uint32_t surplus_radices[] = { 3, 3 };
	static const uint32_t zero_radix[] = { 2, 0, 2 };
	static const uint32_t small_capacity[] = { 2, 2 };
	uint8_t invalid_bits[3] = { 0, 2, 1 };
	uint8_t bits[3] = { 1, 1, 1 };
	uint8_t output[3] = { 7, 7, 7 };
	uint32_t digits[3] = { 11, 12, 13 };
	uint32_t invalid_digits[3] = { 0, 2, 0 };
	uint32_t surplus_digits[2] = { 0, 0 };
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
	state = 1;
	CHECK(mixed_radix_decode_differential(
		      surplus_radices, 2, surplus_digits, &state,
		      output, 3) == MIXED_RADIX_OUT_OF_RANGE &&
	      state == 1 && output[0] == 7 &&
	      output[1] == 7 && output[2] == 7,
	      "unused differential tuple changed state or bits");
	CHECK(mixed_radix_decode_differential(
		      good_radices, 3, invalid_digits, &state,
		      output, 3) == MIXED_RADIX_OUT_OF_RANGE &&
	      state == 1 && output[0] == 7 &&
	      output[1] == 7 && output[2] == 7,
	      "invalid differential digit changed state or bits");
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

static int test_binary_input_boundaries(void)
{
	static const size_t widths[] = { 0, 1, 7, 8, 9, 17, 42, 63, 64, 65, 72, 128 };
	uint32_t radices[MIXED_RADIX_MAX_DIGITS];
	uint32_t digits[MIXED_RADIX_MAX_DIGITS], saved[MIXED_RADIX_MAX_DIGITS];
	uint8_t storage[MIXED_RADIX_MAX_BITS + 7];
	uint8_t decoded[MIXED_RADIX_MAX_BITS + 1];

	for (size_t digit = 0; digit < MIXED_RADIX_MAX_DIGITS; ++digit)
		radices[digit] = 2;
	for (size_t width = 0; width < sizeof(widths) / sizeof(widths[0]); ++width) {
		size_t count = widths[width], digit_count = count != 0 ? count : 1;

		for (size_t offset = 0; offset < 8; ++offset) {
			uint8_t *bits = storage + offset;

			memset(storage, 0xff, sizeof(storage));
			for (size_t bit = 0; bit < count; ++bit)
				bits[bit] = (uint8_t)(bit & 1u);
			CHECK(mixed_radix_encode(radices, digit_count, bits, count, digits) ==
				MIXED_RADIX_OK, "valid bits failed at %zu/%zu", count, offset);
			for (size_t digit = 0; digit < digit_count; ++digit)
				CHECK(digits[digit] == (count != 0 ? bits[digit] : 0),
					"binary digit order changed");
			memset(decoded, 0xa5, sizeof(decoded));
			CHECK(mixed_radix_decode(radices, digit_count, digits, decoded, count) ==
				MIXED_RADIX_OK && decoded[count] == 0xa5 &&
			      memcmp(decoded, bits, count) == 0,
			      "binary inverse changed at %zu/%zu", count, offset);
			memset(digits, 0xa5, sizeof(digits));
			memcpy(saved, digits, sizeof(saved));
			for (size_t bit = 0; bit < count; ++bit) {
				uint8_t state = 1, sign = 7;

				bits[bit] = bit & 1u ? 2u : 255u;
				CHECK(mixed_radix_encode(radices, digit_count, bits, count, digits) ==
					MIXED_RADIX_OUT_OF_RANGE &&
				      mixed_radix_encode_differential(radices, digit_count,
					bits, count, &state, digits, &sign) == MIXED_RADIX_OUT_OF_RANGE &&
				      state == 1 && sign == 7 &&
				      memcmp(digits, saved, sizeof(digits)) == 0,
				      "bad bit changed output/state at %zu/%zu/%zu", count, offset, bit);
				bits[bit] = (uint8_t)(bit & 1u);
			}
		}
	}
	CHECK(mixed_radix_encode(radices, 1, NULL, 0, digits) == MIXED_RADIX_OK &&
	      digits[0] == 0, "empty input must encode zero, including a null input");
	memcpy(saved, digits, sizeof(saved));
	storage[0] = 2;
	CHECK(mixed_radix_encode(radices, 1, NULL, 1, digits) == MIXED_RADIX_INVALID_ARGUMENT &&
	      mixed_radix_encode(radices, 0, storage, 1, digits) == MIXED_RADIX_INVALID_CONFIGURATION &&
	      memcmp(digits, saved, sizeof(digits)) == 0,
	      "argument/configuration rejection precedence changed");
	return 0;
}

int main(void)
{
	CHECK(test_uneven_round_trip() == 0, "uneven round trip failed");
	CHECK(test_wide_round_trip() == 0, "wide round trip failed");
	CHECK(test_received_low_bits() == 0, "received low bits failed");
	CHECK(test_differential_round_trip() == 0,
	      "differential round trip failed");
	CHECK(test_transactionality() == 0, "transactionality failed");
	CHECK(test_binary_input_boundaries() == 0, "binary input boundaries failed");
	CHECK(test_result_names() == 0, "result names failed");
	return 0;
}
