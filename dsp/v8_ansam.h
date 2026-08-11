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
	/* A 200 ms window contains exactly three 15 Hz envelope cycles. */
	V8_ANSAM_DETECTOR_WINDOW_SAMPLES =
		V8_ANSAM_SAMPLE_RATE / 5,
	V8_ANSAM_REQUIRED_WINDOWS = 2,
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

struct v8_ansam_projection {
	double in_phase;
	double quadrature;
	uint32_t phase;
	uint32_t phase_step;
};

struct v8_ansam_detector {
	struct v8_ansam_projection lower_sideband;
	struct v8_ansam_projection carrier;
	struct v8_ansam_projection upper_sideband;
	double line_energy;
	double carrier_share;
	double modulation_ratio;
	uint32_t sample_count;
	unsigned int qualifying_windows;
	unsigned int missed_windows;
	bool measurement_ready;
	bool detected;
};

struct v8_ansam_measurement {
	double carrier_share;
	double modulation_ratio;
	unsigned int qualifying_windows;
	bool ready;
	bool detected;
};

enum v8_ansam_result v8_ansam_generator_init(
	struct v8_ansam_generator *generator,
	int16_t average_amplitude, bool phase_reversals);
enum v8_ansam_result v8_ansam_generate(
	struct v8_ansam_generator *generator,
	int16_t *samples, size_t count);

enum v8_ansam_result v8_ansam_detector_init(
	struct v8_ansam_detector *detector);
void v8_ansam_detector_reset(
	struct v8_ansam_detector *detector);
enum v8_ansam_result v8_ansam_detect(
	struct v8_ansam_detector *detector,
	const int16_t *samples, size_t count, bool *detected);
struct v8_ansam_measurement v8_ansam_measure(
	const struct v8_ansam_detector *detector);

const char *v8_ansam_result_name(
	enum v8_ansam_result result);

#endif
