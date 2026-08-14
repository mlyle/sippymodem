// SPDX-License-Identifier: BSD-3-Clause

#include <phase.h>

#include <math.h>

#include "test.h"

static const double PI = 3.1415926535897932384626433832795;

static bool test_integer_frequency_steps(void)
{
	CHECK(phase_step_u32(8000, 1000, 1) == UINT32_C(0x20000000) &&
	      phase_step_u32(8000, 1200, 1) == UINT32_C(0x26666666) &&
	      phase_step_u32(8000, 1800, 1) == UINT32_C(0x3999999a),
	      "integer-frequency phase steps changed");
	return true;
}

static bool test_rational_frequency_steps(void)
{
	CHECK(phase_step_u32(1, 1, 3) == UINT32_C(0x55555555) &&
	      phase_step_u32(3, 1, 2) == UINT32_C(0x2aaaaaab) &&
	      phase_step_u32(UINT32_MAX, UINT32_MAX, UINT32_MAX) == 1,
	      "rational phase-step rounding changed");
	return true;
}

static bool test_invalid_and_wrapping_steps(void)
{
	CHECK(phase_step_u32(0, 1, 1) == 0 &&
	      phase_step_u32(1, 0, 1) == 0 &&
	      phase_step_u32(1, 1, 0) == 0 &&
	      phase_step_u32(1, 1, 1) == 0 &&
	      phase_step_u32(UINT32_MAX, UINT32_MAX - 1u, 1) == UINT32_MAX,
	      "invalid or full-turn phase-step handling changed");
	return true;
}

static bool test_radian_conversion(void)
{
	CHECK(phase_radians_u32(0) == 0.0 &&
	      fabs(phase_radians_u32(UINT32_C(0x40000000)) - PI / 2.0) <
		      1e-15 &&
	      fabs(phase_radians_u32(UINT32_C(0x80000000)) - PI) < 1e-15 &&
	      phase_radians_u32(UINT32_MAX) < 2.0 * PI,
	      "phase-to-radian conversion changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "integer frequency steps", test_integer_frequency_steps },
		{ "rational frequency steps", test_rational_frequency_steps },
		{ "invalid and wrapping steps", test_invalid_and_wrapping_steps },
		{ "radian conversion", test_radian_conversion },
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
