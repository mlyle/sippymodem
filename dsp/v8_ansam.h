// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_ANSAM_H
#define V8_ANSAM_H

#include "v8_line_conditioner.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V8_ANSAM_SAMPLE_RATE = 8000,
	V8_ANSAM_CARRIER_HZ = 2100,
	V8_ANSAM_MODULATION_HZ = 15,
	V8_ANSAM_REVERSAL_SAMPLES =
		V8_ANSAM_SAMPLE_RATE * 450 / 1000,
	V8_ANSAM_BLOCK_SAMPLES = V8_LINE_BLOCK_SAMPLES,
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
	uint32_t samples_until_reversal;
	int16_t signed_amplitude;
	bool phase_reversals;
};

enum v8_ansam_detector_stage {
	V8_ANSAM_WAIT_CARRIER,
	V8_ANSAM_WAIT_MODULATION,
	V8_ANSAM_DETECTED,
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

/* The detector owns ANSam evidence while its caller supplies the continuous
 * V.8 line conditioner shared with the following control exchange. */
struct v8_ansam_detector {
	struct v8_ansam_tone_detector tone;
	struct v8_ansam_dft modulation;
	uint16_t signal_timer;
	uint16_t signal_ref_gain;
	uint16_t signal_stable_time;
	uint16_t dwell_blocks;
	uint16_t modulation_energy;
	enum v8_ansam_detector_stage stage;
	bool signal_stable;
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
	struct v8_line_conditioner *conditioner,
	const int16_t *samples, size_t count, bool *detected);

#endif
