// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_CP_LINE_TX_H
#define V90_PHASE4_CP_LINE_TX_H

#include "v34_training_line.h"
#include "v90_phase4_cp_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase4_cp_line_tx_result {
	V90_PHASE4_CP_LINE_TX_OK,
	V90_PHASE4_CP_LINE_TX_COMPLETE,
	V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS,
	V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT,
	V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS,
	V90_PHASE4_CP_LINE_TX_INVALID_STATE,
	V90_PHASE4_CP_LINE_TX_OUT_OF_RANGE,
	V90_PHASE4_CP_LINE_TX_CLIPPING_RISK,
	V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR,
};

/* Own CP/CPt symbols and the analogue modem's continuous 8 kHz QAM line.
 * Initialization takes an exact next-point boundary so carrier, pulse shaping,
 * pre-emphasis, and the sample clock continue unchanged from Phase 3. Calls on
 * one instance are serialized. */
struct v90_phase4_cp_line_tx {
	struct v34_training_line line;
	struct v90_phase4_cp_tx symbols;
};

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_init(
	struct v90_phase4_cp_line_tx *tx,
	const struct v34_training_line *line,
	enum v34_constellation_size constellation,
	const struct v90_cp_parameters *training_parameters);
bool v90_phase4_cp_line_tx_is_valid(
	const struct v90_phase4_cp_line_tx *tx);
bool v90_phase4_cp_line_tx_record_is_complete(
	const struct v90_phase4_cp_line_tx *tx);
bool v90_phase4_cp_line_tx_is_handoff_ready(
	const struct v90_phase4_cp_line_tx *tx);

/* Start another record after its predecessor's final point was accepted. The
 * line and symbol-coding histories remain continuous. */
enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_start_next_record(
	struct v90_phase4_cp_line_tx *tx,
	const struct v90_cp_parameters *parameters);

/* Emit one line sample. COMPLETE leaves state and output unchanged at the
 * exact next-point boundary following a completed record. */
enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_sample(
	struct v90_phase4_cp_line_tx *tx, int16_t *sample);

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_handoff(
	const struct v90_phase4_cp_line_tx *tx,
	struct v34_training_line *line);

const char *v90_phase4_cp_line_tx_result_name(
	enum v90_phase4_cp_line_tx_result result);

#endif /* V90_PHASE4_CP_LINE_TX_H */
