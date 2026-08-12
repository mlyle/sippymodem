// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_LINE_TX_H
#define V90_PHASE3_RETURN_LINE_TX_H

#include "v34_training_line.h"
#include "v90_phase3_return_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_return_line_tx_result {
	V90_PHASE3_RETURN_LINE_TX_OK,
	V90_PHASE3_RETURN_LINE_TX_COMPLETE,
	V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT,
	V90_PHASE3_RETURN_LINE_TX_INVALID_STATE,
	V90_PHASE3_RETURN_LINE_TX_OUT_OF_RANGE,
	V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR,
};

/* Continue the analogue modem's live 8 kHz QAM line after Ja. The caller
 * transfers an exact next-point boundary; this owner then emits silence,
 * open-ended S, and S-bar without restarting carrier or pulse shaping. */
struct v90_phase3_return_line_tx {
	struct v34_training_line line;
	struct v90_phase3_return_tx points;
};

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_init(
	struct v90_phase3_return_line_tx *tx,
	const struct v34_training_line *line);
bool v90_phase3_return_line_tx_is_valid(
	const struct v90_phase3_return_line_tx *tx);
bool v90_phase3_return_line_tx_is_handoff_ready(
	const struct v90_phase3_return_line_tx *tx);

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_start_s(
	struct v90_phase3_return_line_tx *tx);
enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_finish_s(
	struct v90_phase3_return_line_tx *tx);
enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_sample(
	struct v90_phase3_return_line_tx *tx, int16_t *sample);

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_handoff(
	const struct v90_phase3_return_line_tx *tx,
	struct v34_training_line *line);

const char *v90_phase3_return_line_tx_result_name(
	enum v90_phase3_return_line_tx_result result);

#endif /* V90_PHASE3_RETURN_LINE_TX_H */
