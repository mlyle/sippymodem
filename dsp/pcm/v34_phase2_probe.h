// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_H
#define V34_PHASE2_PROBE_H

#include "v34_phase2_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE2_PROBE_TONE_SPACING_HZ = 150,
	V34_PHASE2_PROBE_HIGHEST_HARMONIC = 25,
	V34_PHASE2_PROBE_TONE_COUNT = 21,
	/* Three 150 Hz cycles are the shortest whole-sample period at 8 kHz. */
	V34_PHASE2_PROBE_PERIOD_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 3 /
		V34_PHASE2_PROBE_TONE_SPACING_HZ,
	V34_PHASE2_PROBE_L1_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 160 / 1000,
	V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 500 / 1000,
	/* The protocol extends the local-echo limit by the measured round trip. */
	V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 550 / 1000,
};

enum v34_phase2_probe_level {
	V34_PHASE2_PROBE_L1,
	V34_PHASE2_PROBE_L2,
};

enum v34_phase2_probe_result {
	V34_PHASE2_PROBE_OK,
	V34_PHASE2_PROBE_INVALID_ARGUMENT,
};

/* The probing profile contains every 150 Hz harmonic from 150 through
 * 3750 Hz except 900, 1200, 1800, and 2400 Hz. */
static inline bool
v34_phase2_probe_harmonic_is_transmitted(unsigned int harmonic)
{
	return harmonic >= 1u &&
	       harmonic <= V34_PHASE2_PROBE_HIGHEST_HARMONIC &&
	       harmonic != 6u && harmonic != 8u &&
	       harmonic != 12u && harmonic != 16u;
}

struct v34_phase2_probe_transmitter {
	uint16_t period_index;
	int16_t nominal_amplitude;
};

/* nominal_amplitude is the peak level of a sinusoid at nominal transmit
 * power. The aggregate L2 waveform has the same mean-square power; L1 is
 * 6 dB higher. Initialization rejects levels that could clip during L1. */
enum v34_phase2_probe_result
v34_phase2_probe_transmitter_init(
	struct v34_phase2_probe_transmitter *transmitter,
	int16_t nominal_amplitude);
bool v34_phase2_probe_transmitter_is_valid(
	const struct v34_phase2_probe_transmitter *transmitter);

/* Changing level does not reset phase. In particular, the mandatory L1
 * duration ends on a period boundary and can flow directly into L2. */
enum v34_phase2_probe_result
v34_phase2_probe_transmitter_sample(
	struct v34_phase2_probe_transmitter *transmitter,
	enum v34_phase2_probe_level level, int16_t *sample);

#endif
