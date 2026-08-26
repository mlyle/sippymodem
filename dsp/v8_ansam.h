// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_ANSAM_H
#define V8_ANSAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V8_ANSAM_SAMPLE_RATE = 8000,
	V8_ANSAM_CARRIER_HZ = 2100,
	V8_ANSAM_MODULATION_HZ = 15,
	V8_ANSAM_REVERSAL_SAMPLES =
		V8_ANSAM_SAMPLE_RATE * 450 / 1000,
	V8_ANSAM_BLOCK_SAMPLES = 4,
	V8_ANSAM_INPUT_QUEUE_SAMPLES = 6,
	V8_ANSAM_ENTRANCE_FILTER_TAPS = 34,
	V8_ANSAM_POWER_HISTORY_SAMPLES = 30,
	/* Te is one second plus the 30 ms receiver margin. */
	V8_ANSAM_POST_DETECT_DWELL_BLOCKS =
		V8_ANSAM_SAMPLE_RATE / V8_ANSAM_BLOCK_SAMPLES + 60,
	/* The specified envelope peaks at 6/5 of its average amplitude. */
	V8_ANSAM_MAX_AVERAGE_AMPLITUDE = INT16_MAX * 5 / 6,
};

enum v8_ansam_result {
	V8_ANSAM_OK,
	V8_ANSAM_INVALID_ARGUMENT,
};

struct v8_ansam_generator {
	uint32_t carrier_phase;
	uint32_t modulation_phase;
	uint32_t carrier_phase_step;
	uint32_t modulation_phase_step;
	uint32_t samples_until_reversal;
	int16_t average_amplitude;
	bool phase_reversals;
};

enum v8_ansam_detector_stage {
	V8_ANSAM_WAIT_CARRIER,
	V8_ANSAM_WAIT_MODULATION,
	V8_ANSAM_DETECTED,
};

struct v8_ansam_agc {
	uint16_t filtered_energy;
	uint16_t gain;
	uint16_t gain_delta;
	uint16_t power_history[V8_ANSAM_POWER_HISTORY_SAMPLES];
	uint16_t power_index;
	uint16_t signal_timer;
	uint16_t signal_ref_gain;
	uint16_t signal_stable_time;
	uint16_t overflow_count;
	bool signal_stable;
};

struct v8_ansam_tone_detector {
	uint16_t counter;
	uint16_t filtered_energy;
	uint16_t iir_input[4];
	uint16_t iir_output[4];
	uint16_t tone_input[3];
	uint16_t tone_output[3];
	uint16_t warmup_count;
	bool locked;
};

struct v8_ansam_dft {
	uint16_t phase;
	uint32_t real_accumulator;
	uint32_t imaginary_accumulator;
};

/* The receive owner includes the entrance filter, gain lineage, robust
 * carrier acquisition, and the complete post-detect echo-control dwell. */
struct v8_ansam_detector {
	struct v8_ansam_agc agc;
	struct v8_ansam_tone_detector tone;
	struct v8_ansam_dft modulation;
	int16_t entrance_history[V8_ANSAM_ENTRANCE_FILTER_TAPS];
	int16_t input_queue[V8_ANSAM_INPUT_QUEUE_SAMPLES];
	uint16_t post_filter_word;
	uint16_t input_count;
	uint16_t dwell_blocks;
	uint16_t modulation_energy;
	enum v8_ansam_detector_stage stage;
	bool gain_tracking;
};

enum v8_ansam_result v8_ansam_generator_init(
	struct v8_ansam_generator *generator,
	int16_t average_amplitude, bool phase_reversals);
enum v8_ansam_result v8_ansam_generate(
	struct v8_ansam_generator *generator,
	int16_t *samples, size_t count);

enum v8_ansam_result v8_ansam_detector_init(
	struct v8_ansam_detector *detector);
bool v8_ansam_detector_is_valid(
	const struct v8_ansam_detector *detector);
enum v8_ansam_result v8_ansam_detect(
	struct v8_ansam_detector *detector,
	const int16_t *samples, size_t count, bool *detected);

#endif
