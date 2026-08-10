// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_E_SEQUENCE_H
#define SIPPYMODEM_V34_PHASE4_E_SEQUENCE_H

#include "sippymodem_qam.h"
#include "sippymodem_v34_constellation.h"
#include "sippymodem_v34_framing.h"
#include "sippymodem_v34_phase4_mp.h"
#include "sippymodem_v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE4_E_BITS = 20,
	SIPPYMODEM_V34_PHASE4_E_4_POINT_SYMBOLS =
		SIPPYMODEM_V34_PHASE4_E_BITS / 2,
	SIPPYMODEM_V34_PHASE4_E_16_POINT_SYMBOLS =
		SIPPYMODEM_V34_PHASE4_E_BITS / 4,
};

enum sippymodem_v34_phase4_e_sequence_result {
	SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK,
	SIPPYMODEM_V34_PHASE4_E_SEQUENCE_COMPLETE,
	SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase4_e_sequence {
	struct sippymodem_v34_scrambler scrambler;
	enum sippymodem_v34_station_role role;
	enum sippymodem_v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t emitted_bits;
};

/* E follows a completed MP or MP-prime record without resetting the Phase 4
 * scrambler or differential encoder. */
enum sippymodem_v34_phase4_e_sequence_result
sippymodem_v34_phase4_e_sequence_init_from_mp(
	struct sippymodem_v34_phase4_e_sequence *sequence,
	const struct sippymodem_v34_phase4_mp *mp);

bool sippymodem_v34_phase4_e_sequence_is_valid(
	const struct sippymodem_v34_phase4_e_sequence *sequence);
bool sippymodem_v34_phase4_e_sequence_is_complete(
	const struct sippymodem_v34_phase4_e_sequence *sequence);

/* Emit one symbol from the fixed 20-bit sequence. Rejected calls leave state
 * and every output unchanged. */
enum sippymodem_v34_phase4_e_sequence_result
sippymodem_v34_phase4_e_sequence_next(
	struct sippymodem_v34_phase4_e_sequence *sequence,
	struct sippymodem_qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

const char *sippymodem_v34_phase4_e_sequence_result_name(
	enum sippymodem_v34_phase4_e_sequence_result result);

#endif
