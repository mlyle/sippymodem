// SPDX-License-Identifier: BSD-3-Clause

#include <fir.h>

#include "test.h"

#include <limits.h>
#include <string.h>

static int16_t next_sample(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return (int16_t)((int32_t)(*state >> 16) - 32768);
}

static bool test_full_width_convolution(void)
{
	uint32_t random = UINT32_C(0x6352fd7a);
	int16_t history[66], coefficients[64], saved[66];
	int16_t saved_coefficients[64];

	CHECK(fir_shift_s16(NULL, NULL, 0, INT16_MIN) == 0,
	      "empty FIR accessed history or produced a sum");
	for (size_t count = 0; count <= ARRAY_LENGTH(coefficients); ++count) {
		for (size_t trial = 0; trial < 4096; ++trial) {
			int16_t sample = trial % 3 == 0 ? INT16_MIN :
				trial % 3 == 1 ? INT16_MAX : next_sample(&random);
			int64_t sum;

			for (size_t i = 0; i < ARRAY_LENGTH(history); ++i)
				history[i] = next_sample(&random);
			for (size_t i = 0; i < ARRAY_LENGTH(coefficients); ++i)
				coefficients[i] = trial % 3 == 0 ? INT16_MIN :
					trial % 3 == 1 ? INT16_MAX : next_sample(&random);
			memcpy(saved, history, sizeof(saved));
			memcpy(saved_coefficients, coefficients, sizeof(coefficients));
			/* Widen independently; only the final sum wraps to 32 bits. */
			sum = count ? (int64_t)sample * coefficients[0] : 0;
			for (size_t i = 1; i < count; ++i)
				sum += (int64_t)saved[i] * coefficients[i];
			CHECK(fir_shift_s16(history + 1, coefficients, count, sample) ==
				      (uint32_t)sum,
			      "FIR sum changed at count=%zu trial=%zu", count, trial);
			CHECK(memcmp(saved_coefficients, coefficients, sizeof(coefficients)) == 0,
			      "FIR modified coefficients");
			for (size_t i = 0; i < ARRAY_LENGTH(history); ++i) {
				int16_t expected = i == 0 || i > count ? saved[i] :
					i == 1 ? sample : saved[i - 1];
				CHECK(history[i] == expected,
				      "FIR history/guard changed at count=%zu trial=%zu tap=%zu",
				      count, trial, i);
			}
		}
	}
	return true;
}

static bool test_impulse_order(void)
{
	const int16_t coefficients[] = {1, -2, 3, -4, 5};
	int16_t history[ARRAY_LENGTH(coefficients)] = {0};

	for (size_t i = 0; i <= ARRAY_LENGTH(coefficients); ++i) {
		int32_t expected = i < ARRAY_LENGTH(coefficients) ?
			1000 * coefficients[i] : 0;
		CHECK(fir_shift_s16(history, coefficients, ARRAY_LENGTH(coefficients),
				   i == 0 ? 1000 : 0) == (uint32_t)expected,
		      "FIR delayed or reordered impulse at tap=%zu", i);
	}
	return true;
}

int main(void)
{
	return test_full_width_convolution() && test_impulse_order() ? 0 : 1;
}
