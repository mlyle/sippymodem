// SPDX-License-Identifier: BSD-3-Clause

#include <integer_math.h>

#include "test.h"

static bool test_zero_and_symmetry(void)
{
	CHECK(gcd_u64(0, 0) == 0 &&
	      gcd_u64(0, 37) == 37 &&
	      gcd_u64(37, 0) == 37 &&
	      gcd_u64(18, 48) == 6 &&
	      gcd_u64(48, 18) == 6,
	      "GCD zero or symmetry contract changed");
	return true;
}

static bool test_full_width_values(void)
{
	CHECK(gcd_u64(UINT64_MAX, 3) == 3 &&
	      gcd_u64(UINT64_MAX, UINT64_MAX - 1) == 1 &&
	      gcd_u64(UINT64_C(1) << 63, UINT64_C(1) << 32) ==
		      (UINT64_C(1) << 32),
	      "GCD truncated or mishandled full-width values");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "zero and symmetry", test_zero_and_symmetry },
		{ "full-width values", test_full_width_values },
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
