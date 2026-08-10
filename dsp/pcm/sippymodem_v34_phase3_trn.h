// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE3_TRN_H
#define SIPPYMODEM_V34_PHASE3_TRN_H

#include "sippymodem_qam.h"
#include "sippymodem_self_sync_scrambler.h"
#include "sippymodem_v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE3_TRN_MIN_SYMBOLS = 512,
};

enum sippymodem_v34_phase3_trn_result {
	SIPPYMODEM_V34_PHASE3_TRN_OK,
	SIPPYMODEM_V34_PHASE3_TRN_INVALID_ARGUMENT,
};

/* Phase 3 TRN has no intrinsic end marker. The owning startup controller
 * decides when at least 512 symbols have trained the remote receiver. */
struct sippymodem_v34_phase3_trn {
	struct sippymodem_self_sync_scrambler scrambler;
	enum sippymodem_v34_station_role role;
	uint8_t last_clockwise_quarter_turns;
	bool has_last_symbol;
};

enum sippymodem_v34_phase3_trn_result sippymodem_v34_phase3_trn_init(
	struct sippymodem_v34_phase3_trn *trn,
	enum sippymodem_v34_station_role role);
bool sippymodem_v34_phase3_trn_is_valid(
	const struct sippymodem_v34_phase3_trn *trn);

/* Consume two binary ones in transmission order and emit one unit-energy
 * four-point TRN symbol. Rejected calls leave state and outputs unchanged. */
enum sippymodem_v34_phase3_trn_result sippymodem_v34_phase3_trn_next(
	struct sippymodem_v34_phase3_trn *trn,
	struct sippymodem_qam_point *point,
	uint8_t *clockwise_quarter_turns);

const char *sippymodem_v34_phase3_trn_result_name(
	enum sippymodem_v34_phase3_trn_result result);

#endif
