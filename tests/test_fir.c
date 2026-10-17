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

static bool test_read_ring(void)
{
	uint32_t random = UINT32_C(0x1be506ad);
	int16_t history[128], coefficients[128 * 10];
	int16_t saved_history[128], saved_coefficients[128 * 10];

	CHECK(fir_read_ring_s16(NULL, NULL, 0, 0, 1) == 0,
	      "empty ring FIR accessed storage");
	for (size_t count = 1; count <= ARRAY_LENGTH(history); ++count) {
		for (size_t stride = 1; stride <= 10; ++stride) {
			for (size_t trial = 0; trial < 3; ++trial) {
				for (size_t i = 0; i < ARRAY_LENGTH(history); ++i)
					history[i] = trial == 0 ? INT16_MIN :
						trial == 1 ? INT16_MAX : next_sample(&random);
				for (size_t i = 0; i < ARRAY_LENGTH(coefficients); ++i)
					coefficients[i] = trial == 0 ? INT16_MIN :
						trial == 1 ? INT16_MAX : next_sample(&random);
				memcpy(saved_history, history, sizeof(history));
				memcpy(saved_coefficients, coefficients, sizeof(coefficients));
				for (size_t start = 0; start < count; ++start) {
					int64_t expected = 0;
					size_t phase = start % stride;

					/* Independent modular indexing and a widened sum. Only
					 * the final result wraps, including full-scale products. */
					for (size_t tap = 0; tap < count; ++tap)
						expected += (int64_t)history[(start + count - tap) % count] *
							coefficients[phase + tap * stride];
					CHECK(fir_read_ring_s16(history, coefficients + phase, count,
								start, stride) == (uint32_t)expected,
					      "ring FIR changed count=%zu start=%zu stride=%zu trial=%zu",
					      count, start, stride, trial);
				}
				CHECK(memcmp(history, saved_history, sizeof(history)) == 0 &&
				      memcmp(coefficients, saved_coefficients, sizeof(coefficients)) == 0,
				      "read-only ring FIR changed storage");
			}
		}
	}
	return true;
}

static bool test_symmetric_ring(void)
{
	uint32_t random = UINT32_C(0x80cd176a);
	int16_t guarded[82], saved[82], linear[80];
	int16_t coefficients[40], saved_coefficients[40];

	for (size_t count = 2; count <= ARRAY_LENGTH(linear); count += 2) {
		for (size_t start = 0; start < count; ++start) {
			size_t index = start;

			for (size_t i = 0; i < ARRAY_LENGTH(guarded); ++i)
				guarded[i] = start % 3 == 0 ? INT16_MIN :
					start % 3 == 1 ? INT16_MAX : next_sample(&random);
			for (size_t i = 0; i < ARRAY_LENGTH(coefficients); ++i)
				coefficients[i] = start % 3 == 0 ? INT16_MIN :
					start % 3 == 1 ? INT16_MAX : next_sample(&random);
			memcpy(saved_coefficients, coefficients, sizeof(coefficients));
			for (size_t tap = 0; tap < count; ++tap)
				linear[tap] = guarded[1 + (start + count - 1 - tap) % count];
			for (size_t trial = 0; trial < 2 * count; ++trial) {
				int16_t sample = trial % 3 == 0 ? INT16_MIN :
					trial % 3 == 1 ? INT16_MAX : next_sample(&random);
				size_t inserted = index;
				int64_t expected = 0;

				memcpy(saved, guarded, sizeof(guarded));
				memmove(linear + 1, linear, (count - 1) * sizeof(linear[0]));
				linear[0] = sample;
				/* Independent newest-first convolution, not paired arithmetic
				 * or ring traversal. Full-width products must not wrap. */
				for (size_t tap = 0; tap < count; ++tap)
					expected += (int64_t)linear[tap] *
						coefficients[tap < count / 2 ? tap : count - 1 - tap];
				CHECK(fir_ring_symmetric_s16(guarded + 1, coefficients,
							     count, &index, sample) == expected,
				      "symmetric FIR sum changed at count=%zu start=%zu trial=%zu",
				      count, start, trial);
				CHECK(index == (inserted + 1) % count,
				      "symmetric FIR cursor changed at count=%zu start=%zu trial=%zu",
				      count, start, trial);
				saved[inserted + 1] = sample;
				CHECK(memcmp(guarded, saved, sizeof(guarded)) == 0,
				      "symmetric FIR changed another slot or guard");
				CHECK(memcmp(coefficients, saved_coefficients, sizeof(coefficients)) == 0,
				      "symmetric FIR modified coefficients");
			}
		}
	}
	return true;
}

int main(void)
{
	return test_full_width_convolution() && test_impulse_order() &&
	       test_read_ring() && test_symmetric_ring() ? 0 : 1;
}
