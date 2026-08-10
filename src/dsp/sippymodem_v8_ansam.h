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

enum sippymodem_v8_ansam_result sippymodem_v8_ansam_generator_init(
	struct sippymodem_v8_ansam_generator *generator,
	int16_t average_amplitude, bool phase_reversals);
enum sippymodem_v8_ansam_result sippymodem_v8_ansam_generate(
	struct sippymodem_v8_ansam_generator *generator,
	int16_t *samples, size_t count);

const char *sippymodem_v8_ansam_result_name(
	enum sippymodem_v8_ansam_result result);

#endif
