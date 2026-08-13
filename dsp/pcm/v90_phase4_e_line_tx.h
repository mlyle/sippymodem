// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_E_LINE_TX_H
#define V90_PHASE4_E_LINE_TX_H

#include "v34_phase4_e_sequence.h"
#include "v90_phase4_cp_line_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase4_e_line_tx_result {
	V90_PHASE4_E_LINE_TX_OK,
	V90_PHASE4_E_LINE_TX_COMPLETE,
	V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT,
	V90_PHASE4_E_LINE_TX_INVALID_STATE,
	V90_PHASE4_E_LINE_TX_OUT_OF_RANGE,
	V90_PHASE4_E_LINE_TX_CLIPPING_RISK,
	V90_PHASE4_E_LINE_TX_INTERNAL_ERROR,
};

/* Own the analogue modem's continuous QAM line for the 20-bit E sequence.
 * CP/CP-prime scrambler, differential phase, carrier, pulse shaping, and
 * pre-emphasis histories all cross the boundary without reset. */
struct v90_phase4_e_line_tx {
	struct v34_training_line line;
	struct v34_phase4_e_sequence sequence;
};

enum v90_phase4_e_line_tx_result v90_phase4_e_line_tx_init(
	struct v90_phase4_e_line_tx *transmitter,
	const struct v90_phase4_cp_line_tx *completed_cp);
bool v90_phase4_e_line_tx_is_valid(
	const struct v90_phase4_e_line_tx *transmitter);
bool v90_phase4_e_line_tx_is_complete(
	const struct v90_phase4_e_line_tx *transmitter);

/* Produce one 8 kHz line sample. COMPLETE leaves state and output untouched
 * at the exact next-point boundary after E. */
enum v90_phase4_e_line_tx_result v90_phase4_e_line_tx_sample(
	struct v90_phase4_e_line_tx *transmitter, int16_t *sample);

enum v90_phase4_e_line_tx_result v90_phase4_e_line_tx_handoff(
	const struct v90_phase4_e_line_tx *transmitter,
	struct v34_training_line *line);

const char *v90_phase4_e_line_tx_result_name(
	enum v90_phase4_e_line_tx_result result);

#endif /* V90_PHASE4_E_LINE_TX_H */
