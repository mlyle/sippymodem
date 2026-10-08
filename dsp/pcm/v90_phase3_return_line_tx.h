// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_LINE_TX_H
#define V90_PHASE3_RETURN_LINE_TX_H

#include "v34_training_line.h"
#include "v34_training_sequence.h"
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

/* Continue the analogue modem's live 8 kHz QAM line after Ja or online data.
 * The Phase 3 entries take an exact next-point boundary and emit silence or
 * SCR, open-ended S, and S-bar.  The rate-renegotiation entry first drains an
 * already accepted point and schedules S/S-bar automatically.  Neither mode
 * restarts the carrier or pulse shaper. */
struct v90_phase3_return_line_tx {
	struct v34_training_line line;
	struct v90_phase3_return_tx points;
	struct v34_training_sequence idle_scr;
	bool scr_during_idle;
	bool automatic_rrn;
	bool draining_accepted_point;
};

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_init(
	struct v90_phase3_return_line_tx *tx,
	const struct v34_training_line *line);
enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_init_scr(
	struct v90_phase3_return_line_tx *tx,
	const struct v34_training_line *line,
	enum v34_constellation_size constellation);

/* Take an online data line at any sample position.  The already accepted
 * point drains without accepting another data point, then the transmitter
 * emits the V.90 rate-renegotiation response of at least 128T S followed by
 * exactly 16T S-bar.  COMPLETE remains the exact next-point boundary after
 * the final S-bar point. */
enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_init_rrn(
	struct v90_phase3_return_line_tx *tx,
	const struct v34_training_line *online_line);
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

#endif /* V90_PHASE3_RETURN_LINE_TX_H */
