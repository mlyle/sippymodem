// SPDX-License-Identifier: BSD-3-Clause

#include <packed_bits.h>

#include "test.h"

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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire order", test_wire_order },
		{ "byte boundaries", test_byte_boundaries },
		{ "zero tail", test_zero_tail },
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
