// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_S_TRANSMITTER_H
#define V34_PHASE3_S_TRANSMITTER_H

#include "v34_phase3_s_sequence.h"
#include "v34_training_line.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_S_PULSE_SPAN_SYMBOLS =
		V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS,
	V34_PHASE3_S_TAIL_SYMBOLS =
		V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS,
	V34_PHASE3_S_FILTER_TAIL_SAMPLES =
		V34_TRAINING_LINE_FILTER_TAIL_SAMPLES,
};

enum v34_phase3_s_transmitter_stage {
	V34_PHASE3_S_TRANSMIT_SYMBOLS,
	V34_PHASE3_S_TRANSMIT_PULSE_TAIL,
	V34_PHASE3_S_TRANSMIT_FILTER_TAIL,
	V34_PHASE3_S_TRANSMIT_COMPLETE,
};

enum v34_phase3_s_transmitter_result {
	V34_PHASE3_S_TRANSMITTER_OK,
	V34_PHASE3_S_TRANSMITTER_COMPLETE,
	V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT,
	V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK,
	V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE,
};

struct v34_phase3_s_transmitter {
	struct v34_training_line line;
	struct v34_phase3_s_sequence sequence;
	uint16_t tail_symbols;
	uint8_t filter_tail_samples;
	enum v34_phase3_s_transmitter_stage stage;
};

/* The nominal amplitude is the peak amplitude of an equal-power sinusoid.
 * Initialization applies the profile's negotiated reduction and calibrates
 * the finite S/S-bar marker to the resulting RMS power. */
enum v34_phase3_s_transmitter_result
v34_phase3_s_transmitter_init(
	struct v34_phase3_s_transmitter *transmitter,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool v34_phase3_s_transmitter_is_valid(
	const struct v34_phase3_s_transmitter *transmitter);
bool v34_phase3_s_transmitter_is_complete(
	const struct v34_phase3_s_transmitter *transmitter);

/* Successful calls emit one line sample. The complete and error results leave
 * both transmitter and output unchanged. */
enum v34_phase3_s_transmitter_result
v34_phase3_s_transmitter_sample(
	struct v34_phase3_s_transmitter *transmitter,
	int16_t *sample);

const char *v34_phase3_s_transmitter_result_name(
	enum v34_phase3_s_transmitter_result result);

#endif
