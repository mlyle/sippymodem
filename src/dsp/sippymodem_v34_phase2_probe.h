// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_H
#define SIPPYMODEM_V34_PHASE2_PROBE_H

#include "sippymodem_v34_phase2_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_PROBE_TONE_SPACING_HZ = 150,
	SIPPYMODEM_V34_PHASE2_PROBE_TONE_COUNT = 21,
	/* Three 150 Hz cycles are the shortest whole-sample period at 8 kHz. */
	SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 3 /
		SIPPYMODEM_V34_PHASE2_PROBE_TONE_SPACING_HZ,
	SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 160 / 1000,
	/* The protocol may extend this L2 limit by the measured round-trip delay. */
	SIPPYMODEM_V34_PHASE2_PROBE_L2_BASE_LIMIT_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 550 / 1000,
};

enum sippymodem_v34_phase2_probe_level {
	SIPPYMODEM_V34_PHASE2_PROBE_L1,
	SIPPYMODEM_V34_PHASE2_PROBE_L2,
};

enum sippymodem_v34_phase2_probe_result {
	SIPPYMODEM_V34_PHASE2_PROBE_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_probe_transmitter {
	uint16_t period_index;
	int16_t nominal_amplitude;
};

/* nominal_amplitude is the peak level of a sinusoid at nominal transmit
 * power. The aggregate L2 waveform has the same mean-square power; L1 is
 * 6 dB higher. Initialization rejects levels that could clip during L1. */
enum sippymodem_v34_phase2_probe_result
sippymodem_v34_phase2_probe_transmitter_init(
	struct sippymodem_v34_phase2_probe_transmitter *transmitter,
	int16_t nominal_amplitude);
bool sippymodem_v34_phase2_probe_transmitter_is_valid(
	const struct sippymodem_v34_phase2_probe_transmitter *transmitter);

/* Changing level does not reset phase. In particular, the mandatory L1
 * duration ends on a period boundary and can flow directly into L2. */
enum sippymodem_v34_phase2_probe_result
sippymodem_v34_phase2_probe_transmitter_sample(
	struct sippymodem_v34_phase2_probe_transmitter *transmitter,
	enum sippymodem_v34_phase2_probe_level level, int16_t *sample);

const char *sippymodem_v34_phase2_probe_result_name(
	enum sippymodem_v34_phase2_probe_result result);

#endif
