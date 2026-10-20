// SPDX-License-Identifier: BSD-3-Clause

#include <phase.h>
#include <wide_uint.h>

#include <float.h>
#include <math.h>

#include "test.h"

static const double PI = 3.1415926535897932384626433832795;

static bool check_advance(uint64_t initial, uint64_t period, uint64_t step)
{
	struct wide_uint sum;
	CHECK(wide_add(wide_from_u64(initial), wide_from_u64(step), &sum),
	      "phase oracle overflowed its wide accumulator");
	bool expected_crossing = wide_compare(sum, wide_from_u64(period)) >= 0;
	struct wide_uint expected = expected_crossing ?
		wide_subtract(sum, wide_from_u64(period)) : sum;
	uint64_t phase = initial, without_fraction = initial;
	double fraction = -1.;
	CHECK(phase_advance_u64(&phase, period, step, &fraction) == expected_crossing &&
	      phase_advance_u64(&without_fraction, period, step, NULL) == expected_crossing &&
	      expected.high == 0 && phase == expected.low && without_fraction == phase,
	      "rational phase changed at initial=%llu period=%llu step=%llu",
	      (unsigned long long)initial, (unsigned long long)period, (unsigned long long)step);
	if (expected_crossing) {
		double reference = (double)((long double)(period - initial) / step);
		CHECK(fraction > 0. && fraction <= 1. &&
		      fabs(fraction - reference) <= 4. * DBL_EPSILON,
		      "crossing fraction escaped the current sample");
	} else {
		CHECK(fraction == -1., "noncrossing sample changed the fraction output");
	}
	return true;
}

static bool test_rational_phase_advance(void)
{
	/* Sum first in an independent wide integer, including cases where a
	 * native phase+step would overflow. Exhaust small periods and all their
	 * residuals, with both absent and present interpolation outputs. */
	for (uint64_t period = 1; period <= 96; ++period)
		for (uint64_t initial = 0; initial < period; ++initial)
			for (uint64_t step = 0; step <= period; ++step)
				CHECK(check_advance(initial, period, step), "small phase failed");
	const uint64_t periods[] = { 8000, UINT32_MAX, UINT64_C(1) << 63,
		(UINT64_C(1) << 63) + 17, UINT64_MAX };
	for (size_t i = 0; i < ARRAY_LENGTH(periods); ++i) {
		uint64_t period = periods[i];
		const uint64_t positions[] = { 0, 1, period / 2, period - 1 };
		const uint64_t steps[] = { 0, 1, period / 2, period - 1, period };
		for (size_t p = 0; p < ARRAY_LENGTH(positions); ++p)
			for (size_t s = 0; s < ARRAY_LENGTH(steps); ++s)
				CHECK(check_advance(positions[p], period, steps[s]), "wide phase failed");
	}
	return true;
}

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

static bool test_sine_samples(void)
{
	const int16_t amplitudes[] = { 0, 1, 3, 127, 8192, INT16_MAX };
	const uint32_t phases[] = {
		0, UINT32_C(0x40000000), UINT32_C(0x80000000),
		UINT32_C(0xc0000000), UINT32_MAX,
	};
	const int signs[] = { 0, 1, 0, -1, 0 };
	const long double turn = 2.0L * acosl(-1.0L);

	for (size_t a = 0; a < ARRAY_LENGTH(amplitudes); ++a) {
		int16_t amplitude = amplitudes[a];

		for (size_t p = 0; p < ARRAY_LENGTH(phases); ++p)
			CHECK(phase_sine_s16(phases[p], amplitude) == signs[p] * amplitude,
			      "sine endpoint changed at phase=%u amplitude=%d", phases[p], amplitude);
		/* Independent extended-precision oracle across a complete turn,
		 * including values immediately on either side of a half sample. */
		for (uint32_t p = 0; p < 65536; ++p) {
			uint32_t phase = p * UINT32_C(65537);
			long expected = lroundl(amplitude * sinl(turn * phase / 4294967296.0L));

			CHECK(phase_sine_s16(phase, amplitude) == expected,
			      "sine sample changed at phase=%u amplitude=%d", phase, amplitude);
		}
	}
	CHECK(phase_sine_s16(UINT32_C(0x15555555), 1) == 0 &&
	      phase_sine_s16(UINT32_C(0x15555556), 1) == 1 &&
	      phase_sine_s16(UINT32_C(0x95555555), 1) == 0 &&
	      phase_sine_s16(UINT32_C(0x95555556), 1) == -1,
	      "sine rounding changed beside positive or negative half samples");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "rational phase advance", test_rational_phase_advance },
		{ "integer frequency steps", test_integer_frequency_steps },
		{ "rational frequency steps", test_rational_frequency_steps },
		{ "invalid and wrapping steps", test_invalid_and_wrapping_steps },
		{ "radian conversion", test_radian_conversion },
		{ "sine samples", test_sine_samples },
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
