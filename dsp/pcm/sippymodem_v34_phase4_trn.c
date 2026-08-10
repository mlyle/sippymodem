// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase4_trn.h"

#include <stddef.h>

bool sippymodem_v34_phase4_trn_is_valid(
	const struct sippymodem_v34_phase4_trn *trn)
{
	if (trn == NULL ||
	    !sippymodem_v34_constellation_size_is_valid(trn->constellation) ||
	    trn->last_clockwise_quarter_turns > 3u ||
	    (!trn->has_last_symbol &&
	     trn->last_clockwise_quarter_turns != 0u))
		return false;
	return sippymodem_v34_transmit_scrambler_is_valid(
		&trn->scrambler, trn->role);
}

enum sippymodem_v34_phase4_trn_result sippymodem_v34_phase4_trn_init(
	struct sippymodem_v34_phase4_trn *trn,
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_constellation_size constellation)
{
	struct sippymodem_v34_phase4_trn initialized = {
		.role = role,
		.constellation = constellation,
	};

	if (trn == NULL ||
	    !sippymodem_v34_constellation_size_is_valid(constellation))
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	if (sippymodem_v34_transmit_scrambler_init(
		    &initialized.scrambler, role) !=
	    SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	*trn = initialized;
	return SIPPYMODEM_V34_PHASE4_TRN_OK;
}

enum sippymodem_v34_phase4_trn_result sippymodem_v34_phase4_trn_next(
	struct sippymodem_v34_phase4_trn *trn,
	struct sippymodem_qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct sippymodem_v34_phase4_trn next;
	struct sippymodem_qam_point next_point;
	uint8_t next_quarter_point = 0;
	uint8_t rotation;

	if (!sippymodem_v34_phase4_trn_is_valid(trn) || point == NULL ||
	    quarter_point == NULL || clockwise_quarter_turns == NULL)
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	next = *trn;
	if (sippymodem_v34_scramble_mapped_dibit(
		    &next.scrambler, 3u, &rotation) !=
	    SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	if (next.constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT &&
	    sippymodem_v34_scramble_mapped_dibit(
		    &next.scrambler, 3u, &next_quarter_point) !=
		    SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	if (sippymodem_v34_training_constellation_point(
		    next.constellation, next_quarter_point, rotation,
		    &next_point) != SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	next.last_clockwise_quarter_turns = rotation;
	next.has_last_symbol = true;
	if (!sippymodem_v34_phase4_trn_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT;
	*trn = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = rotation;
	return SIPPYMODEM_V34_PHASE4_TRN_OK;
}

const char *sippymodem_v34_phase4_trn_result_name(
	enum sippymodem_v34_phase4_trn_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE4_TRN_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
