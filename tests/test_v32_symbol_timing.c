// SPDX-License-Identifier: BSD-3-Clause

#include <symbol_timing.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	STREAM_SAMPLES = 720,
	OUTPUT_SAMPLES = 800,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static uint64_t hash_byte(uint64_t hash, uint8_t byte)
{
	return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t hash_u16(uint64_t hash, uint16_t word)
{
	hash = hash_byte(hash, (uint8_t)word);
	return hash_byte(hash, (uint8_t)(word >> 8));
}

static uint64_t hash_u32(uint64_t hash, uint32_t word)
{
	hash = hash_u16(hash, (uint16_t)word);
	return hash_u16(hash, (uint16_t)(word >> 16));
}

static uint64_t hash_words(
	uint64_t hash, const int16_t *values, size_t count)
{
	for (size_t index = 0; index < count; ++index)
		hash = hash_u16(hash, (uint16_t)values[index]);
	return hash;
}

static uint64_t hash_i16(const int16_t *values, size_t count)
{
	return hash_words(
		UINT64_C(14695981039346656037), values, count);
}

static uint64_t hash_state(const struct v32_symbol_timing *timing)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	hash = hash_words(hash, timing->interpolator,
			  V32_SYMBOL_TIMING_COEFFICIENTS);
	hash = hash_words(hash, timing->sample_history,
			  V32_SYMBOL_TIMING_HISTORY);
	hash = hash_words(hash, timing->error_state,
			  V32_SYMBOL_TIMING_ERROR_STATE);
	hash = hash_words(hash, timing->startup_samples,
			  V32_SYMBOL_TIMING_STARTUP_SAMPLES);
	hash = hash_u32(hash, (uint32_t)timing->detector_i_accumulator);
	hash = hash_u32(hash, (uint32_t)timing->detector_q_accumulator);
	hash = hash_u16(hash, (uint16_t)timing->loop_integral);
	hash = hash_u16(hash, (uint16_t)timing->smoothed_error);
	hash = hash_u16(hash, (uint16_t)timing->detector_level);
	hash = hash_u16(hash, (uint16_t)timing->fractional_phase_q11);
	hash = hash_u16(hash, (uint16_t)timing->polyphase_index);
	hash = hash_u16(hash, timing->history_index);
	hash = hash_u16(hash, timing->acquisition_groups);
	hash = hash_u16(hash, timing->detector_groups);
	hash = hash_u16(hash, timing->detector_sample);
	hash = hash_u16(hash, timing->samples_until_output);
	hash = hash_u16(hash, timing->startup_index);
	hash = hash_u16(hash, (uint16_t)timing->mode);
	hash = hash_u16(hash, timing->signal_detected);
	hash = hash_u16(hash, timing->tracking_enabled);
	return hash_u16(hash, timing->startup_gate_enabled);
}

static void fill_stream(int16_t *samples, size_t count)
{
	uint32_t state = UINT32_C(0x13579bdf);

	for (size_t index = 0; index < count; ++index) {
		int32_t noise = (int32_t)(fixture_random(&state) >> 17) - 16384;
		double tone = 10500.0 * sin(
			2.0 * 3.14159265358979323846 * 1200.0 *
			(double)index / 7200.0);

		samples[index] = (int16_t)lround(tone + noise * 0.125);
	}
}

static bool test_profiles_and_impulse(void)
{
	static const int16_t expected_impulse[] = {
		44, -90, 146, -210, 278, -348, 408, -456, 486,
		17442, 486, -456, 408, -348, 278, -210, 146, -90,
	};
	struct v32_symbol_timing timing;
	int16_t input[V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES] = {0};
	int16_t output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES] = {0};
	uint64_t atan_hash = UINT64_C(14695981039346656037);
	size_t produced;

	/* Exhaustively lock the private generated 257-word atan profile. */
	for (unsigned index = 0; index <= 256; ++index) {
		uint16_t word = (uint16_t)lround(
			atan((double)index / 256.0) * 32768.0);

		atan_hash ^= word & UINT16_C(0xff);
		atan_hash *= UINT64_C(1099511628211);
		atan_hash ^= word >> 8;
		atan_hash *= UINT64_C(1099511628211);
	}
	CHECK(atan_hash == UINT64_C(0x7c49943f7a8914b5),
	      "generated timing atan profile changed");
	CHECK(v32_symbol_timing_init(&timing) == V32_SYMBOL_TIMING_OK &&
	      hash_i16(timing.interpolator,
		       V32_SYMBOL_TIMING_COEFFICIENTS) ==
		      UINT64_C(0xf05422f5e7cbfd1a),
	      "timing interpolator profile changed");
	timing.tracking_enabled = false;
	timing.startup_gate_enabled = false;
	timing.signal_detected = true;
	input[0] = 16384;
	CHECK(v32_symbol_timing_process(
		      &timing, input, ARRAY_LENGTH(input), output,
		      ARRAY_LENGTH(output), &produced) == V32_SYMBOL_TIMING_OK &&
	      produced == ARRAY_LENGTH(input) &&
	      memcmp(output, expected_impulse, sizeof(expected_impulse)) == 0,
	      "timing phase-zero impulse response changed");
	for (size_t index = ARRAY_LENGTH(expected_impulse);
	     index < produced; ++index)
		CHECK(output[index] == 0,
		      "timing impulse tail was nonzero at %zu", index);

	/* A fine-loop step exercises every adjacent source-table pair. */
	CHECK(v32_symbol_timing_init(&timing) == V32_SYMBOL_TIMING_OK,
	      "fine timing profile owner did not initialize");
	timing.mode = V32_SYMBOL_TIMING_FINE;
	timing.acquisition_groups = 71;
	timing.startup_gate_enabled = false;
	timing.signal_detected = true;
	timing.detector_level = 3;
	timing.detector_groups = 15;
	timing.detector_sample = 2;
	timing.detector_i_accumulator = INT32_C(4096) << 16;
	timing.detector_q_accumulator = INT32_C(128) << 16;
	input[0] = 0;
	CHECK(v32_symbol_timing_process(
		      &timing, input, 1, output, ARRAY_LENGTH(output),
		      &produced) == V32_SYMBOL_TIMING_OK &&
	      produced == 1 && timing.mode == V32_SYMBOL_TIMING_FINE &&
	      timing.fractional_phase_q11 == 2034 &&
	      timing.polyphase_index == 9 &&
	      hash_i16(timing.interpolator,
		       V32_SYMBOL_TIMING_COEFFICIENTS) ==
		      UINT64_C(0xf422488c6076b2a5),
	      "fine timing interpolation profile changed");
	return true;
}

static bool run_stream(
	struct v32_symbol_timing *timing,
	const int16_t *samples, size_t count,
	const size_t *chunks, size_t chunk_count,
	int16_t *output, size_t *output_count)
{
	size_t consumed = 0;
	size_t produced = 0;
	size_t chunk = 0;

	while (consumed < count) {
		size_t current = chunks[chunk++ % chunk_count];
		size_t emitted;

		if (current > count - consumed)
			current = count - consumed;
		CHECK(current <= V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES &&
		      produced <= OUTPUT_SAMPLES,
		      "timing test buffer bound failed");
		CHECK(v32_symbol_timing_process(
			      timing, &samples[consumed], current,
			      &output[produced], OUTPUT_SAMPLES - produced,
			      &emitted) == V32_SYMBOL_TIMING_OK,
		      "timing owner rejected chunk %zu", current);
		consumed += current;
		produced += emitted;
	}
	*output_count = produced;
	return true;
}

static bool test_fragmentation_and_golden(void)
{
	static const size_t epochs[] = {
		V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES,
	};
	static const size_t fragments[] = { 1, 2, 17, 5, 36, 3, 11 };
	struct v32_symbol_timing whole;
	struct v32_symbol_timing split;
	int16_t input[STREAM_SAMPLES];
	int16_t whole_output[OUTPUT_SAMPLES] = {0};
	int16_t split_output[OUTPUT_SAMPLES] = {0};
	size_t whole_count;
	size_t split_count;

	fill_stream(input, ARRAY_LENGTH(input));
	CHECK(v32_symbol_timing_init(&whole) == V32_SYMBOL_TIMING_OK &&
	      v32_symbol_timing_init(&split) == V32_SYMBOL_TIMING_OK &&
	      run_stream(
		      &whole, input, ARRAY_LENGTH(input), epochs,
		      ARRAY_LENGTH(epochs), whole_output, &whole_count) &&
	      run_stream(
		      &split, input, ARRAY_LENGTH(input), fragments,
		      ARRAY_LENGTH(fragments), split_output, &split_count),
	      "timing fragmentation fixture failed");
	CHECK(whole_count == split_count && whole_count > 0 &&
	      memcmp(whole_output, split_output,
		     whole_count * sizeof(whole_output[0])) == 0 &&
	      memcmp(&whole, &split, sizeof(whole)) == 0,
	      "timing output or owner depended on caller fragmentation");
	/* Filled from an independent exact timing oracle. */
	CHECK(whole_count == 723 &&
	      hash_i16(whole_output, whole_count) ==
		      UINT64_C(0x4052e9d51556f4b5) &&
	      hash_state(&whole) == UINT64_C(0xe754ffeae26f8bce),
	      "timing output or terminal state diverged from its oracle");
	return true;
}

static void prepare_detector_boundary(
	struct v32_symbol_timing *timing, uint8_t groups)
{
	timing->startup_gate_enabled = false;
	timing->signal_detected = true;
	timing->detector_level = 3;
	timing->detector_groups = groups;
	timing->detector_sample = 2;
	timing->detector_i_accumulator = INT32_C(64) << 16;
	timing->detector_q_accumulator = 0;
}

static bool clock_once(struct v32_symbol_timing *timing)
{
	int16_t input = 0;
	int16_t output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	size_t produced;

	return v32_symbol_timing_process(
		       timing, &input, 1, output, ARRAY_LENGTH(output),
		       &produced) == V32_SYMBOL_TIMING_OK &&
	       produced >= 1;
}

static bool test_modes_and_hysteresis(void)
{
	struct v32_symbol_timing timing;

	CHECK(v32_symbol_timing_init(&timing) == V32_SYMBOL_TIMING_OK,
	      "timing mode fixture did not initialize");
	prepare_detector_boundary(&timing, 2);
	timing.acquisition_groups = 69;
	CHECK(clock_once(&timing) && timing.acquisition_groups == 70 &&
	      timing.mode == V32_SYMBOL_TIMING_ACQUISITION,
	      "timing acquisition moved before group 71");
	prepare_detector_boundary(&timing, 2);
	CHECK(clock_once(&timing) && timing.acquisition_groups == 71 &&
	      timing.mode == V32_SYMBOL_TIMING_COARSE,
	      "timing acquisition did not end at group 71");
	prepare_detector_boundary(&timing, 2);
	CHECK(clock_once(&timing) && timing.mode == V32_SYMBOL_TIMING_FINE,
	      "stable timing did not enter fine tracking");

	prepare_detector_boundary(&timing, 15);
	timing.smoothed_error = 2000;
	timing.detector_q_accumulator = INT32_C(4096) << 16;
	CHECK(clock_once(&timing) && timing.mode == V32_SYMBOL_TIMING_COARSE,
	      "large timing error did not leave fine tracking");

	CHECK(v32_symbol_timing_init(&timing) == V32_SYMBOL_TIMING_OK,
	      "timing hysteresis fixture did not initialize");
	timing.tracking_enabled = false;
	prepare_detector_boundary(&timing, 2);
	CHECK(clock_once(&timing) && timing.signal_detected,
	      "timing level two did not assert signal");
	prepare_detector_boundary(&timing, 2);
	timing.detector_level = 1;
	CHECK(clock_once(&timing) && !timing.signal_detected,
	      "timing level one did not clear signal");
	return true;
}

static size_t worst_cursor_outputs(
	uint8_t samples_until_output, size_t outputs_until_correction)
{
	size_t remaining = V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES;
	size_t produced = 0;
	size_t needed = samples_until_output;

	while (remaining != 0) {
		if (remaining < needed)
			break;
		remaining -= needed;
		produced++;
		if (--outputs_until_correction == 0) {
			needed = 0;
			outputs_until_correction = 9;
		} else {
			needed = 1;
		}
	}
	return produced;
}

static bool test_output_bound(void)
{
	struct v32_symbol_timing timing;
	size_t maximum = 0;

	for (enum v32_symbol_timing_mode mode =
		     V32_SYMBOL_TIMING_ACQUISITION;
	     mode <= V32_SYMBOL_TIMING_FINE; ++mode) {
		uint8_t limit = mode == V32_SYMBOL_TIMING_FINE ? 16 : 3;

		for (uint8_t groups = 0; groups < limit; ++groups) {
			for (uint8_t sample = 0; sample < 3; ++sample) {
				for (uint8_t needed = 0; needed <= 2; ++needed) {
					size_t until =
						(size_t)(limit - groups) * 3u - sample;
					size_t bound =
						worst_cursor_outputs(needed, until);

					CHECK(v32_symbol_timing_init(&timing) ==
						      V32_SYMBOL_TIMING_OK,
					      "timing bound owner did not initialize");
					timing.mode = mode;
					timing.detector_groups = groups;
					timing.detector_sample = sample;
					timing.samples_until_output = needed;
					CHECK(v32_symbol_timing_is_valid(&timing) &&
					      bound <=
						      V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES,
					      "valid timing cursor exceeded output bound");
					if (bound > maximum)
						maximum = bound;
				}
			}
		}
	}
	CHECK(maximum == V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES,
	      "timing output bound was not tight: %zu", maximum);
	return true;
}

static bool test_contracts(void)
{
	struct v32_symbol_timing timing;
	struct v32_symbol_timing saved;
	int16_t input[V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES];
	int16_t saved_input[V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES];
	int16_t output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	int16_t saved_output[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	size_t produced = 123;
	size_t saved_produced;

	fill_stream(input, ARRAY_LENGTH(input));
	memset(output, 0x5c, sizeof(output));
	memcpy(saved_input, input, sizeof(input));
	memcpy(saved_output, output, sizeof(output));
	CHECK(v32_symbol_timing_init(&timing) == V32_SYMBOL_TIMING_OK,
	      "timing contract fixture did not initialize");
	saved = timing;
	saved_produced = produced;
#define CHECK_REJECT(call, expected, label)                                \
	CHECK((call) == (expected) &&                                         \
	      memcmp(&timing, &saved, sizeof(timing)) == 0 &&                  \
	      memcmp(input, saved_input, sizeof(input)) == 0 &&                \
	      memcmp(output, saved_output, sizeof(output)) == 0 &&             \
	      produced == saved_produced, "%s was not transactional", label)
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, NULL, 1, output, ARRAY_LENGTH(output),
			     &produced),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "null timing input");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, input, ARRAY_LENGTH(input) + 1u, output,
			     ARRAY_LENGTH(output), &produced),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "oversize timing input");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, input, ARRAY_LENGTH(input), NULL, 0,
			     &produced),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "short timing output");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, (const int16_t *)(const void *)&timing, 1,
			     output, ARRAY_LENGTH(output), &produced),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "timing input/owner alias");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, input, ARRAY_LENGTH(input),
			     (int16_t *)(void *)&timing, ARRAY_LENGTH(output),
			     &produced),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "timing output/owner alias");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, input, ARRAY_LENGTH(input), input,
			     ARRAY_LENGTH(input), &produced),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "timing input/output alias");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, input, ARRAY_LENGTH(input), output,
			     ARRAY_LENGTH(output), (size_t *)(void *)&timing),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "timing count/owner alias");
	CHECK_REJECT(v32_symbol_timing_process(
			     &timing, input, ARRAY_LENGTH(input), output,
			     ARRAY_LENGTH(output), NULL),
		     V32_SYMBOL_TIMING_INVALID_ARGUMENT, "null timing output count");
#undef CHECK_REJECT
	CHECK(v32_symbol_timing_process(
		      NULL, input, 1, output, ARRAY_LENGTH(output), &produced) ==
		      V32_SYMBOL_TIMING_INVALID_STATE &&
	      v32_symbol_timing_init(NULL) ==
		      V32_SYMBOL_TIMING_INVALID_ARGUMENT,
	      "null timing owner was accepted");
	timing.polyphase_index = 10;
	saved = timing;
	CHECK(v32_symbol_timing_process(
		      &timing, input, 1, output, ARRAY_LENGTH(output),
		      &produced) == V32_SYMBOL_TIMING_INVALID_STATE &&
	      memcmp(&timing, &saved, sizeof(timing)) == 0,
	      "invalid timing owner changed during rejection");
	return true;
}

int main(void)
{
	if (!test_profiles_and_impulse() || !test_fragmentation_and_golden() ||
	    !test_modes_and_hysteresis() || !test_output_bound() ||
	    !test_contracts())
		return 1;
	puts("V.32 symbol timing tests passed");
	return 0;
}
