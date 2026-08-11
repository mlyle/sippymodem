// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_ANSAM_H
#define SIPPYMODEM_V8_ANSAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V8_ANSAM_SAMPLE_RATE = 8000,
	SIPPYMODEM_V8_ANSAM_CARRIER_HZ = 2100,
	SIPPYMODEM_V8_ANSAM_MODULATION_HZ = 15,
	SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES =
		SIPPYMODEM_V8_ANSAM_SAMPLE_RATE * 450 / 1000,
	/* A 200 ms window contains exactly three 15 Hz envelope cycles. */
	SIPPYMODEM_V8_ANSAM_DETECTOR_WINDOW_SAMPLES =
		SIPPYMODEM_V8_ANSAM_SAMPLE_RATE / 5,
	SIPPYMODEM_V8_ANSAM_REQUIRED_WINDOWS = 2,
	/* The specified envelope peaks at 6/5 of its average amplitude. */
	SIPPYMODEM_V8_ANSAM_MAX_AVERAGE_AMPLITUDE = INT16_MAX * 5 / 6,
};

enum sippymodem_v8_ansam_result {
	SIPPYMODEM_V8_ANSAM_OK,
	SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT,
};

struct sippymodem_v8_ansam_generator {
	uint32_t carrier_phase;
	uint32_t modulation_phase;
	uint32_t carrier_phase_step;
	uint32_t modulation_phase_step;
	uint32_t samples_until_reversal;
	int16_t average_amplitude;
	bool phase_reversals;
};

struct sippymodem_v8_ansam_projection {
	double in_phase;
	double quadrature;
	uint32_t phase;
	uint32_t phase_step;
};

struct sippymodem_v8_ansam_detector {
	struct sippymodem_v8_ansam_projection lower_sideband;
	struct sippymodem_v8_ansam_projection carrier;
	struct sippymodem_v8_ansam_projection upper_sideband;
	double line_energy;
	double carrier_share;
	double modulation_ratio;
	uint32_t sample_count;
	unsigned int qualifying_windows;
	unsigned int missed_windows;
	bool measurement_ready;
	bool detected;
};

struct sippymodem_v8_ansam_measurement {
	double carrier_share;
	double modulation_ratio;
	unsigned int qualifying_windows;
	bool ready;
	bool detected;
};

enum sippymodem_v8_ansam_result sippymodem_v8_ansam_generator_init(
	struct sippymodem_v8_ansam_generator *generator,
	int16_t average_amplitude, bool phase_reversals);
enum sippymodem_v8_ansam_result sippymodem_v8_ansam_generate(
	struct sippymodem_v8_ansam_generator *generator,
	int16_t *samples, size_t count);

enum sippymodem_v8_ansam_result sippymodem_v8_ansam_detector_init(
	struct sippymodem_v8_ansam_detector *detector);
void sippymodem_v8_ansam_detector_reset(
	struct sippymodem_v8_ansam_detector *detector);
enum sippymodem_v8_ansam_result sippymodem_v8_ansam_detect(
	struct sippymodem_v8_ansam_detector *detector,
	const int16_t *samples, size_t count, bool *detected);
struct sippymodem_v8_ansam_measurement sippymodem_v8_ansam_measure(
	const struct sippymodem_v8_ansam_detector *detector);

const char *sippymodem_v8_ansam_result_name(
	enum sippymodem_v8_ansam_result result);

#endif
