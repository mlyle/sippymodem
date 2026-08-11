// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_TRN_H
#define SIPPYMODEM_V34_PHASE4_TRN_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE4_TRN_MIN_SYMBOLS = 512,
};

enum sippymodem_v34_phase4_trn_result {
	SIPPYMODEM_V34_PHASE4_TRN_OK,
	SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT,
};

/* Phase 4 TRN has no intrinsic end marker. The startup controller owns its
 * minimum duration and the role-specific maximum that includes round-trip
 * delay for an answering station. */
struct sippymodem_v34_phase4_trn {
	struct sippymodem_v34_scrambler scrambler;
	enum sippymodem_v34_station_role role;
	enum sippymodem_v34_constellation_size constellation;
	uint8_t last_clockwise_quarter_turns;
	bool has_last_symbol;
};

enum sippymodem_v34_phase4_trn_result sippymodem_v34_phase4_trn_init(
	struct sippymodem_v34_phase4_trn *trn,
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_constellation_size constellation);
bool sippymodem_v34_phase4_trn_is_valid(
	const struct sippymodem_v34_phase4_trn *trn);

/* Consume binary ones in transmission order and emit one selected training
 * symbol. The final rotation is retained to initialize MP differential phase.
 * Rejected calls leave state and outputs unchanged. */
enum sippymodem_v34_phase4_trn_result sippymodem_v34_phase4_trn_next(
	struct sippymodem_v34_phase4_trn *trn,
	struct sippymodem_qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

const char *sippymodem_v34_phase4_trn_result_name(
	enum sippymodem_v34_phase4_trn_result result);

#endif
