// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_H
#define SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_H

#include "sippymodem_qam.h"
#include "sippymodem_v34_phase3_profile.h"
#include "sippymodem_v34_phase3_s_sequence.h"
#include "sippymodem_v34_preemphasis.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE3_S_PULSE_SPAN_SYMBOLS = 8,
	SIPPYMODEM_V34_PHASE3_S_TAIL_SYMBOLS =
		SIPPYMODEM_V34_PHASE3_S_PULSE_SPAN_SYMBOLS,
	SIPPYMODEM_V34_PHASE3_S_FILTER_TAIL_SAMPLES =
		SIPPYMODEM_V34_PREEMPHASIS_TAPS - 1,
};

enum sippymodem_v34_phase3_s_transmitter_stage {
	SIPPYMODEM_V34_PHASE3_S_TRANSMIT_SYMBOLS,
	SIPPYMODEM_V34_PHASE3_S_TRANSMIT_PULSE_TAIL,
	SIPPYMODEM_V34_PHASE3_S_TRANSMIT_FILTER_TAIL,
	SIPPYMODEM_V34_PHASE3_S_TRANSMIT_COMPLETE,
};

enum sippymodem_v34_phase3_s_transmitter_result {
	SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_OK,
	SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_COMPLETE,
	SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK,
	SIPPYMODEM_V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE,
};

struct sippymodem_v34_phase3_s_transmitter {
	struct sippymodem_v34_phase3_profile profile;
	struct sippymodem_qam_modulator carrier;
	struct sippymodem_v34_preemphasis preemphasis;
	struct sippymodem_v34_phase3_s_sequence sequence;
	double output_gain;
	double raw_rms;
	double raw_peak;
	double target_rms;
	uint32_t generated_samples;
	uint16_t tail_symbols;
	uint8_t filter_tail_samples;
	int16_t nominal_peak_amplitude;
	enum sippymodem_v34_phase3_s_transmitter_stage stage;
};

/* The nominal amplitude is the peak amplitude of an equal-power sinusoid.
 * Initialization applies the profile's negotiated reduction and calibrates
 * the finite S/S-bar marker to the resulting RMS power. */
enum sippymodem_v34_phase3_s_transmitter_result
sippymodem_v34_phase3_s_transmitter_init(
	struct sippymodem_v34_phase3_s_transmitter *transmitter,
	const struct sippymodem_v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool sippymodem_v34_phase3_s_transmitter_is_valid(
	const struct sippymodem_v34_phase3_s_transmitter *transmitter);
bool sippymodem_v34_phase3_s_transmitter_is_complete(
	const struct sippymodem_v34_phase3_s_transmitter *transmitter);

/* Successful calls emit one line sample. The complete and error results leave
 * both transmitter and output unchanged. */
enum sippymodem_v34_phase3_s_transmitter_result
sippymodem_v34_phase3_s_transmitter_sample(
	struct sippymodem_v34_phase3_s_transmitter *transmitter,
	int16_t *sample);

const char *sippymodem_v34_phase3_s_transmitter_result_name(
	enum sippymodem_v34_phase3_s_transmitter_result result);

#endif
