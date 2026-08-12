// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_TX_H
#define V90_PHASE3_RETURN_TX_H

#include "qam.h"
#include "v34_phase3_s_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_return_tx_stage {
	V90_PHASE3_RETURN_TX_SILENCE,
	V90_PHASE3_RETURN_TX_S,
	V90_PHASE3_RETURN_TX_S_BAR,
	V90_PHASE3_RETURN_TX_COMPLETE,
};

enum v90_phase3_return_tx_result {
	V90_PHASE3_RETURN_TX_OK,
	V90_PHASE3_RETURN_TX_DONE,
	V90_PHASE3_RETURN_TX_INVALID_ARGUMENT,
	V90_PHASE3_RETURN_TX_INVALID_STATE,
	V90_PHASE3_RETURN_TX_OUT_OF_RANGE,
};

/* Own the analogue modem's return signal after Ja: silence until a valid Jd
 * is received, open-ended S for at least 128T, then exactly 16T of S-bar once
 * Jd-prime is received. S is extended by one symbol when necessary so it ends
 * on its counterclockwise 90-degree point. Controls affect the next
 * unconsumed point. */
struct v90_phase3_return_tx {
	enum v90_phase3_return_tx_stage stage;
	uint32_t silence_symbols;
	uint32_t s_symbols;
	uint8_t s_bar_symbols;
	bool finish_s_requested;
};

enum v90_phase3_return_tx_result v90_phase3_return_tx_init(
	struct v90_phase3_return_tx *tx);
bool v90_phase3_return_tx_is_valid(
	const struct v90_phase3_return_tx *tx);
bool v90_phase3_return_tx_is_complete(
	const struct v90_phase3_return_tx *tx);

enum v90_phase3_return_tx_result v90_phase3_return_tx_start_s(
	struct v90_phase3_return_tx *tx);
enum v90_phase3_return_tx_result v90_phase3_return_tx_finish_s(
	struct v90_phase3_return_tx *tx);

enum v90_phase3_return_tx_result v90_phase3_return_tx_next(
	struct v90_phase3_return_tx *tx, struct qam_point *point);

const char *v90_phase3_return_tx_result_name(
	enum v90_phase3_return_tx_result result);

#endif /* V90_PHASE3_RETURN_TX_H */
