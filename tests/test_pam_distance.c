// SPDX-License-Identifier: BSD-3-Clause

#include "pam_distance.h"
#include "test.h"

#include <stdbool.h>
#include <string.h>

/* Enumerate both signs and every point, independently of search brackets
 * and uniform-spacing arithmetic. Subtract the cell width only afterward. */
static double exhaustive(const int16_t *points, unsigned count, double x, double width)
{
	double nearest = INFINITY;
	for (unsigned i = 0; i < count; ++i) {
		nearest = fmin(nearest, fabs(x - points[i]));
		nearest = fmin(nearest, fabs(x + points[i]));
	}
	double error = nearest - width;
	return error > 0 ? error * error : 0;
}

static bool check_observation(const int16_t *points, unsigned count, double step,
	const struct pam_distance_range *ranges, double x)
{
	const double widths[] = { 0, .25, 64 };
	for (unsigned i = 0; i < ARRAY_LENGTH(widths); ++i) {
		double expected = exhaustive(points, count, x, widths[i]);
		double actual = pam_distance_squared(points, count, step,
			step > 0 ? 1. / step : 0, widths[i], x, ranges);
		CHECK(fabs(actual - expected) <= 1e-12 * fmax(1., expected),
		      "distance mismatch: count=%u x=%.17g width=%g got=%.17g expected=%.17g",
		      count, x, widths[i], actual, expected);
	}
	return true;
}

static bool check_alphabet(const int16_t *points, unsigned count, double expected_step)
{
	struct pam_distance_range ranges[PAM_DISTANCE_BINS];
	memset(ranges, 0xff, sizeof(ranges));
	double step = -1, inverse_step = -1;
	pam_distance_prepare(points, count, &step, &inverse_step, ranges);
	CHECK(step == expected_step, "incorrect uniform step: %g expected %g", step, expected_step);
	CHECK(inverse_step == (step > 0 ? 1. / step : 0), "incorrect reciprocal step");
	for (unsigned bin = 0; bin < PAM_DISTANCE_BINS; ++bin) {
		unsigned low = ranges[bin].low, high = ranges[bin].high;
		int lower = (int)(bin << PAM_DISTANCE_BIN_SHIFT);
		int upper = lower + (1 << PAM_DISTANCE_BIN_SHIFT);
		if (step > 0 || count == 1) {
			CHECK(low == UINT8_MAX && high == UINT8_MAX, "preparation touched unused brackets");
		} else {
			CHECK(low <= high && high < count &&
			      (low == 0 || points[low] <= lower) &&
			      (low + 1 == count || points[low + 1] > lower) &&
			      (high + 1 == count || points[high] >= upper) &&
			      (high == 0 || points[high - 1] < upper),
			      "invalid bracket: bin=%u range=%u..%u count=%u", bin, low, high, count);
		}
		const double offsets[] = { -.25, 0, .25, 63.75, 64 };
		for (unsigned i = 0; i < ARRAY_LENGTH(offsets); ++i)
			for (int sign = -1; sign <= 1; sign += 2)
				CHECK(check_observation(points, count, step, ranges,
				      sign * (lower + offsets[i])), "bin boundary failed");
	}
	for (unsigned i = 0; i < count; ++i) {
		double values[] = { points[i], i ? .5 * (points[i - 1] + points[i]) : 0 };
		for (unsigned j = 0; j < ARRAY_LENGTH(values); ++j) {
			double near[] = { nextafter(values[j], -INFINITY), values[j],
				nextafter(values[j], INFINITY) };
			for (unsigned k = 0; k < ARRAY_LENGTH(near); ++k)
				for (int sign = -1; sign <= 1; sign += 2)
					CHECK(check_observation(points, count, step, ranges, sign * near[k]),
					      "point/midpoint boundary failed");
		}
	}
	const double outside[] = { -1e150, -65536, -.0, 0, 65536, 1e150 };
	for (unsigned i = 0; i < ARRAY_LENGTH(outside); ++i)
		CHECK(check_observation(points, count, step, ranges, outside[i]), "outside alphabet failed");
	return true;
}

int main(void)
{
	double step = -1, inverse_step = -1;
	pam_distance_prepare(NULL, 0, &step, &inverse_step, NULL);
	if (step != 0 || inverse_step != 0)
		return 1;
	const int16_t singleton[] = { 32767 };
	const int16_t endpoints[] = { 1, 32767 };
	const int16_t uniform[] = { 3, 10, 17, 24, 31 };
	const int16_t clustered[] = { 1, 2, 3, 63, 64, 65, 96, 127, 128, 129, 1000, 32766, 32767 };
	int16_t maximum[256];
	for (unsigned i = 0; i < ARRAY_LENGTH(maximum); ++i)
		maximum[i] = i < 128 ? (int16_t)(i + 1) : (int16_t)(256 * (i - 128) + 255);
	if (!check_alphabet(singleton, ARRAY_LENGTH(singleton), 0) ||
	    !check_alphabet(endpoints, ARRAY_LENGTH(endpoints), 32766) ||
	    !check_alphabet(uniform, ARRAY_LENGTH(uniform), 7) ||
	    !check_alphabet(clustered, ARRAY_LENGTH(clustered), 0) ||
	    !check_alphabet(maximum, ARRAY_LENGTH(maximum), 0))
		return 1;
	puts("PAM distance: owned preparation and scoring match exhaustive signed alphabets");
	return 0;
}
