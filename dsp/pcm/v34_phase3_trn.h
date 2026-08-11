// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_TRN_H
#define V34_PHASE3_TRN_H

#include "qam.h"
#include "v34_framing.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_TRN_MIN_SYMBOLS = 512,
};

enum v34_phase3_trn_result {
	V34_PHASE3_TRN_OK,
	V34_PHASE3_TRN_INVALID_ARGUMENT,
};

/* Phase 3 TRN has no intrinsic end marker. The owning startup controller
 * decides when at least 512 symbols have trained the remote receiver. */
struct v34_phase3_trn {
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	uint8_t last_clockwise_quarter_turns;
	bool has_last_symbol;
};

enum v34_phase3_trn_result v34_phase3_trn_init(
	struct v34_phase3_trn *trn,
	enum v34_station_role role);
bool v34_phase3_trn_is_valid(
	const struct v34_phase3_trn *trn);

/* Consume two binary ones in transmission order and emit one unit-energy
 * four-point TRN symbol. Rejected calls leave state and outputs unchanged. */
enum v34_phase3_trn_result v34_phase3_trn_next(
	struct v34_phase3_trn *trn,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns);

const char *v34_phase3_trn_result_name(
	enum v34_phase3_trn_result result);

#endif
