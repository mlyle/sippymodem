// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_TRN_H
#define V34_PHASE4_TRN_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE4_TRN_MIN_SYMBOLS = 512,
};

enum v34_phase4_trn_result {
	V34_PHASE4_TRN_OK,
	V34_PHASE4_TRN_INVALID_ARGUMENT,
};

/* Phase 4 TRN has no intrinsic end marker. The startup controller owns its
 * minimum duration and the role-specific maximum that includes round-trip
 * delay for an answering station. */
struct v34_phase4_trn {
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	enum v34_constellation_size constellation;
	uint8_t last_clockwise_quarter_turns;
	bool has_last_symbol;
};

enum v34_phase4_trn_result v34_phase4_trn_init(
	struct v34_phase4_trn *trn,
	enum v34_station_role role,
	enum v34_constellation_size constellation);
bool v34_phase4_trn_is_valid(
	const struct v34_phase4_trn *trn);

/* Consume binary ones in transmission order and emit one selected training
 * symbol. The final rotation is retained to initialize MP differential phase.
 * Rejected calls leave state and outputs unchanged. */
enum v34_phase4_trn_result v34_phase4_trn_next(
	struct v34_phase4_trn *trn,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

/* Emit the same selected symbol in the persistent fixed receive coordinates.
 * This keeps known Phase 4 TRN adaptation in the same coordinate system as
 * the receive equalizer without converting through normalized doubles. */
enum v34_phase4_trn_result v34_phase4_trn_next_fixed(
	struct v34_phase4_trn *trn,
	struct v34_analytic_sample *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

const char *v34_phase4_trn_result_name(
	enum v34_phase4_trn_result result);

#endif
