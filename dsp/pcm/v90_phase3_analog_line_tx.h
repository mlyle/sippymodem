// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_ANALOG_LINE_TX_H
#define V90_PHASE3_ANALOG_LINE_TX_H

#include "v34_training_line.h"
#include "v90_phase3.h"
#include "v90_phase3_analog_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE3_ANALOG_LEADING_SILENCE_SAMPLES =
		70 * V90_PHASE3_SAMPLE_RATE / 1000,
};

enum v90_phase3_analog_line_tx_result {
	V90_PHASE3_ANALOG_LINE_TX_OK,
	V90_PHASE3_ANALOG_LINE_TX_COMPLETE,
	V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT,
	V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE,
	V90_PHASE3_ANALOG_LINE_TX_OUT_OF_RANGE,
	V90_PHASE3_ANALOG_LINE_TX_CLIPPING_RISK,
	V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR,
};

/* Own the required 70 ms silence followed by the analogue Phase 3 points on
 * one continuous 8 kHz QAM line. Calls on one instance are serialized. */
struct v90_phase3_analog_line_tx {
	struct v34_training_line line;
	struct v90_phase3_analog_tx symbols;
	uint16_t leading_silence_samples;
};

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_init(
	struct v90_phase3_analog_line_tx *tx,
	const struct v34_phase3_profile *analog_profile,
	int16_t nominal_peak_amplitude);
bool v90_phase3_analog_line_tx_is_valid(
	const struct v90_phase3_analog_line_tx *tx);

/* Controls affect the next point not yet accepted by the line. */
enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_start_ja(
	struct v90_phase3_analog_line_tx *tx);
enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_finish_ja(
	struct v90_phase3_analog_line_tx *tx);

/* Emit one sample. COMPLETE leaves state and output unchanged at the exact
 * next-point boundary after the final accepted Ja symbol. */
enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_sample(
	struct v90_phase3_analog_line_tx *tx, int16_t *sample);

bool v90_phase3_analog_line_tx_is_handoff_ready(
	const struct v90_phase3_analog_line_tx *tx);
enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_handoff(
	const struct v90_phase3_analog_line_tx *tx,
	struct v34_training_line *line);

const char *v90_phase3_analog_line_tx_result_name(
	enum v90_phase3_analog_line_tx_result result);

#endif /* V90_PHASE3_ANALOG_LINE_TX_H */
