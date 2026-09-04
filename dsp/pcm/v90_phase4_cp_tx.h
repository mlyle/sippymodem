// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_CP_TX_H
#define V90_PHASE4_CP_TX_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_station.h"
#include "v90_cp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_PHASE4_CP_MIN_4_POINT_SYMBOLS =
		V90_CP_MIN_FRAME_BITS / 2,
	V90_PHASE4_CP_MAX_4_POINT_SYMBOLS =
		V90_CP_MAX_FRAME_BITS / 2,
	V90_PHASE4_CP_MIN_16_POINT_SYMBOLS =
		V90_CP_MIN_FRAME_BITS / 4,
	V90_PHASE4_CP_MAX_16_POINT_SYMBOLS =
		V90_CP_MAX_FRAME_BITS / 4,
};

enum v90_phase4_cp_tx_result {
	V90_PHASE4_CP_TX_OK,
	V90_PHASE4_CP_TX_RECORD_COMPLETE,
	V90_PHASE4_CP_TX_RECORD_IN_PROGRESS,
	V90_PHASE4_CP_TX_INVALID_ARGUMENT,
	V90_PHASE4_CP_TX_INVALID_PARAMETERS,
};

struct v90_phase4_cp_tx {
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	enum v34_constellation_size constellation;
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t previous_clockwise_quarter_turns;
};

/* Initialize the first CPt with zero scrambler history and zero differential
 * phase. A CP data record is rejected here and may begin only at a later
 * completed-record boundary. The transmitter owns a canonical encoding. */
enum v90_phase4_cp_tx_result v90_phase4_cp_tx_init(
	struct v90_phase4_cp_tx *tx,
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	const struct v90_cp_parameters *parameters);
bool v90_phase4_cp_tx_is_valid(
	const struct v90_phase4_cp_tx *tx);
bool v90_phase4_cp_tx_record_is_complete(
	const struct v90_phase4_cp_tx *tx);

/* Replace a completed record while preserving the continuous scrambler and
 * differential histories required across repeated CPt, CP, and CP-prime. */
enum v90_phase4_cp_tx_result v90_phase4_cp_tx_start_next_record(
	struct v90_phase4_cp_tx *tx,
	const struct v90_cp_parameters *parameters);

/* Emit one 2D training-constellation symbol. record_complete accompanies the
 * last symbol. Rejected calls leave state and every output unchanged. */
enum v90_phase4_cp_tx_result v90_phase4_cp_tx_next(
	struct v90_phase4_cp_tx *tx,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete);

/* Emit one SCR symbol after a completed CPt. SCR is scrambled binary ones
 * using the same continuous differential and scrambler histories as CPt and
 * the following CP. Rejected calls leave state and every output unchanged. */
enum v90_phase4_cp_tx_result v90_phase4_cp_tx_scr_next(
	struct v90_phase4_cp_tx *tx,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

const char *v90_phase4_cp_tx_result_name(
	enum v90_phase4_cp_tx_result result);

#endif /* V90_PHASE4_CP_TX_H */
