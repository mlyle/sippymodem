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

/* Compute each fast window directly from absolute sample indices. The
 * predictor spans +/-radius around input[n-radius]; its input history needs
 * only that delay. Exercise both rings wrapping at different positions. */
static void check_windows(struct echo_timing *timing, unsigned seed)
{
	enum { SAMPLES = 4096 };
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
			assert(timing->fast.first.input == input_energy);
			assert(timing->fast.first.current_cross == cross[ECHO_TIMING_RADIUS]);
			assert(timing->fast.first.current_energy == energy[ECHO_TIMING_RADIUS]);
			count = 0;
			input_energy = 0;
			memset(cross, 0, sizeof(cross));
			memset(energy, 0, sizeof(energy));
		}
		assert(timing->fast.count == count);
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
	timing.received_write = ECHO_TIMING_RADIUS;
	assert(!echo_timing_is_valid(&timing));
	puts("Echo timing delay and window tests passed");
	return 0;
}
