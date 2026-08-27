// SPDX-License-Identifier: BSD-3-Clause

#include <packed_bits.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_wire_order(void)
{
	static const uint8_t packed[] = { 0xa5, 0x3c };
	static const uint8_t expected[] = {
		1, 0, 1, 0, 0, 1, 0, 1,
		0, 0, 1, 1, 1, 1, 0, 0,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(expected); ++index)
		CHECK(packed_msb_bit(packed, index) == expected[index],
		      "packed bit %zu has the wrong wire order", index);
	return true;
}

static bool test_byte_boundaries(void)
{
	uint8_t packed[3];

	for (size_t one = 0; one < sizeof(packed) * 8u; ++one) {
		memset(packed, 0, sizeof(packed));
		packed[one / 8u] = (uint8_t)(0x80u >> (one % 8u));
		for (size_t index = 0; index < sizeof(packed) * 8u; ++index)
			CHECK(packed_msb_bit(packed, index) == (index == one),
			      "bit %zu aliased bit %zu", one, index);
	}
	return true;
}

static bool test_assignment(void)
{
	uint8_t packed[] = { 0xff, 0x00 };

	for (size_t bit = 0; bit < sizeof(packed) * 8u; ++bit)
		packed_msb_set_bit(packed, bit, (bit % 3u) == 1u);
	for (size_t bit = 0; bit < sizeof(packed) * 8u; ++bit)
		CHECK(packed_msb_bit(packed, bit) == ((bit % 3u) == 1u),
		      "assigned bit %zu has the wrong value", bit);
	return true;
}

static uint32_t field_mask(size_t bit_count)
{
	return bit_count == 32u ? UINT32_MAX :
	       (UINT32_C(1) << bit_count) - 1u;
}

static bool test_fields(void)
{
	static const uint32_t values[] = {
		0, UINT32_MAX, UINT32_C(0xa5c31e79),
	};

	for (size_t first_bit = 0; first_bit < 16u; ++first_bit) {
		for (size_t bit_count = 0; bit_count <= 32u; ++bit_count) {
			for (size_t selection = 0;
			     selection < ARRAY_LENGTH(values); ++selection) {
				uint8_t lsb[8];
				uint8_t msb[8];
				uint8_t expected[8];
				uint32_t value = values[selection] &
					field_mask(bit_count);

				memset(lsb, 0xa5, sizeof(lsb));
				memcpy(msb, lsb, sizeof(msb));
				memcpy(expected, lsb, sizeof(expected));
				for (size_t bit = 0; bit < bit_count; ++bit) {
					packed_msb_set_bit(
						expected, first_bit + bit,
						(value & (UINT32_C(1) << bit)) != 0);
				}
				packed_msb_write_lsb_field(
					lsb, first_bit, bit_count, values[selection]);
				CHECK(memcmp(lsb, expected, sizeof(lsb)) == 0 &&
				      packed_msb_read_lsb_field(
					      lsb, first_bit, bit_count) == value,
				      "LSB field differed at %zu/%zu/%zu",
				      first_bit, bit_count, selection);
				memcpy(expected, msb, sizeof(expected));
				for (size_t bit = 0; bit < bit_count; ++bit) {
					packed_msb_set_bit(
						expected, first_bit + bit,
						(value & (UINT32_C(1) <<
							  (bit_count - bit - 1u))) != 0);
				}
				packed_msb_write_msb_field(
					msb, first_bit, bit_count, values[selection]);
				CHECK(memcmp(msb, expected, sizeof(msb)) == 0 &&
				      packed_msb_read_msb_field(
					      msb, first_bit, bit_count) == value,
				      "MSB field differed at %zu/%zu/%zu",
				      first_bit, bit_count, selection);
			}
		}
	}
	return true;
}

static bool test_unpacked_validation(void)
{
	uint8_t bits[] = { 0, 1, 0, 1 };

	CHECK(unpacked_bits_are_binary(bits, ARRAY_LENGTH(bits)),
	      "binary span was rejected");
	CHECK(unpacked_bits_are_binary(bits, 0),
	      "empty nonnull span was rejected");
	bits[2] = 2;
	CHECK(!unpacked_bits_are_binary(bits, ARRAY_LENGTH(bits)),
	      "nonbinary span was accepted");
	CHECK(!unpacked_bits_are_binary(NULL, 0),
	      "null span was accepted");
	return true;
}

static bool test_zero_tail(void)
{
	uint8_t packed[3];

	for (size_t used = 0; used <= sizeof(packed) * 8u; ++used) {
		memset(packed, 0, sizeof(packed));
		for (size_t bit = 0; bit < used; ++bit)
			packed[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
		CHECK(packed_msb_tail_is_zero(packed, used, sizeof(packed)),
		      "valid %zu-bit prefix has a nonzero tail", used);
		if (used < sizeof(packed) * 8u) {
			packed[used / 8u] |=
				(uint8_t)(0x80u >> (used % 8u));
			CHECK(!packed_msb_tail_is_zero(
				      packed, used, sizeof(packed)),
			      "tail bit %zu was accepted", used);
		}
	}
	CHECK(!packed_msb_tail_is_zero(
		      packed, sizeof(packed) * 8u + 1u, sizeof(packed)),
	      "out-of-range used length was accepted");
	CHECK(!packed_msb_tail_is_zero(NULL, 0, 0),
	      "null storage was accepted");
	return true;
}

static bool test_zero_range(void)
{
	uint8_t packed[] = { 0xff, 0x00, 0x08 };

	CHECK(packed_msb_bits_are_zero(packed, 8, 12) &&
	      packed_msb_bits_are_zero(packed, 4, 0) &&
	      !packed_msb_bits_are_zero(packed, 0, 1) &&
	      !packed_msb_bits_are_zero(packed, 8, 13) &&
	      !packed_msb_bits_are_zero(NULL, 0, 0),
	      "packed zero-range validation changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire order", test_wire_order },
		{ "byte boundaries", test_byte_boundaries },
		{ "assignment", test_assignment },
		{ "field order and boundaries", test_fields },
		{ "unpacked validation", test_unpacked_validation },
		{ "zero tail", test_zero_tail },
		{ "zero range", test_zero_range },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
