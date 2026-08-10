// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_TONE_H
#define SIPPYMODEM_V34_PHASE2_TONE_H

#include "sippymodem_v34_phase2_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase2_tone_result {
	SIPPYMODEM_V34_PHASE2_TONE_OK,
	SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_tone_transmitter {
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	uint32_t guard_phase;
	uint32_t guard_phase_step;
	int16_t carrier_amplitude;
	int16_t guard_amplitude;
	enum sippymodem_v34_phase2_station_role local_role;
};

/* Tone B is the calling station's 1200 Hz carrier. Tone A is the answering
 * station's 2400 Hz carrier at -1 dB plus a continuous 1800 Hz guard at the
 * nominal level. nominal_amplitude is the peak level before those gains. */
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_init(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude);
bool sippymodem_v34_phase2_tone_transmitter_is_valid(
	const struct sippymodem_v34_phase2_tone_transmitter *transmitter);

/* Reverse only the carrier. The answering station's guard remains phase
 * continuous, as required for transitions between Tone A states. */
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_reverse(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter);
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_sample(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter,
	int16_t *sample);

const char *sippymodem_v34_phase2_tone_result_name(
	enum sippymodem_v34_phase2_tone_result result);

#endif
