// SPDX-License-Identifier: BSD-3-Clause

#include "echo_timing.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool reject_retime(void *owner, int delay)
{
	(void)owner;
	(void)delay;
	return false;
}

/* Recompute completed evidence directly from the input, independently of
 * the folding used between window scales. */
static void check_evidence(const struct echo_timing_window *window,
	const int16_t *input, const int16_t *prediction, unsigned end,
	unsigned samples)
{
	struct echo_timing_evidence expected = { 0 };

	for (unsigned n = end + 1 - samples; n <= end; n++) {
		int32_t delayed = input[n - ECHO_TIMING_RADIUS];
		int32_t candidate = prediction[n - window->candidate];
		int32_t current = prediction[n - ECHO_TIMING_RADIUS];

		expected.cross += (int64_t)delayed * candidate;
		expected.current_cross += (int64_t)delayed * current;
		expected.energy += (int64_t)candidate * candidate;
		expected.current_energy += (int64_t)current * current;
		expected.input += (int64_t)delayed * delayed;
	}
	assert(window->second);
	assert(window->first.cross == expected.cross);
	assert(window->first.current_cross == expected.current_cross);
	assert(window->first.energy == expected.energy);
	assert(window->first.current_energy == expected.current_energy);
	assert(window->first.input == expected.input);
}

/* Compute each fast window directly from absolute sample indices. The
 * predictor spans +/-radius around input[n-radius]; its input history needs
 * only that delay. Exercise both rings wrapping at different positions. */
static void check_windows(struct echo_timing *timing, unsigned seed)
{
	enum { SAMPLES = 3 * ECHO_TIMING_WINDOW + ECHO_TIMING_BINS };
	int16_t input[SAMPLES], prediction[SAMPLES];
	int64_t cross[ECHO_TIMING_BINS] = { 0 };
	uint64_t energy[ECHO_TIMING_BINS] = { 0 }, input_energy = 0;
	unsigned count = 0;
	struct echo_timing_model model = { .retime = reject_retime };

	for (unsigned n = 0; n < SAMPLES; n++) {
		seed = seed * 1664525u + 1013904223u;
		input[n] = (int)(seed % 16001u) - 8000;
		seed = seed * 1664525u + 1013904223u;
		prediction[n] = (int)(seed % 2001u) - 1000;
		if (n + 1 >= ECHO_TIMING_BINS) {
			int16_t delayed = input[n - ECHO_TIMING_RADIUS];

			input_energy += (int32_t)delayed * delayed;
			for (unsigned i = 0; i < ECHO_TIMING_BINS; i++) {
				int16_t predicted = prediction[n - i];

				cross[i] += (int32_t)delayed * predicted;
				energy[i] += (int32_t)predicted * predicted;
			}
			count++;
		}
		echo_timing_observe(timing, &model, input[n],
			(int16_t)(input[n] - prediction[n]));
		assert(echo_timing_is_valid(timing));
		if (count == ECHO_TIMING_FAST_WINDOW) {
			check_evidence(&timing->fast, input, prediction, n,
				ECHO_TIMING_FAST_WINDOW);
			if ((n + 2 - ECHO_TIMING_BINS) % ECHO_TIMING_MEDIUM_WINDOW == 0)
				check_evidence(&timing->medium, input, prediction, n,
					ECHO_TIMING_MEDIUM_WINDOW);
			if ((n + 2 - ECHO_TIMING_BINS) % ECHO_TIMING_WINDOW == 0)
				check_evidence(&timing->slow, input, prediction, n,
					ECHO_TIMING_WINDOW);
			assert(timing->fast.first.input == input_energy);
			assert(timing->fast.first.current_cross == cross[ECHO_TIMING_RADIUS]);
			assert(timing->fast.first.current_energy == energy[ECHO_TIMING_RADIUS]);
			count = 0;
			input_energy = 0;
			memset(cross, 0, sizeof(cross));
			memset(energy, 0, sizeof(energy));
		}
		assert(timing->count % ECHO_TIMING_FAST_WINDOW == count);
		assert(timing->count == (n + 1 < ECHO_TIMING_BINS ? 0 :
			(n + 2 - ECHO_TIMING_BINS) % ECHO_TIMING_WINDOW));
		assert(timing->fast.input == input_energy);
		assert(memcmp(timing->fast.cross, cross, sizeof(cross)) == 0);
		assert(memcmp(timing->fast.energy, energy, sizeof(energy)) == 0);
	}
}

int main(void)
{
	struct echo_timing timing = { 0 }, zero = { 0 };
	struct echo_timing_model model = { .retime = reject_retime };

	check_windows(&timing, 0x21b5u);
	echo_timing_observe(&timing, &model, INT16_MAX, 0);
	assert(memcmp(&timing, &zero, sizeof(timing)) == 0);
	check_windows(&timing, 0x72c9u);
	echo_timing_observe(&timing, &model, 0, INT16_MIN);
	assert(memcmp(&timing, &zero, sizeof(timing)) == 0);
	check_windows(&timing, 0x8ef3u);
	timing.count = ECHO_TIMING_WINDOW;
	assert(!echo_timing_is_valid(&timing));
	timing.count = 0;
	timing.received_write = ECHO_TIMING_RADIUS;
	assert(!echo_timing_is_valid(&timing));
	puts("Echo timing delay and window tests passed");
	return 0;
}
