// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_tx.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <stddef.h>

bool v90_phase3_return_tx_is_valid(
	const struct v90_phase3_return_tx *tx)
{
	if (tx == NULL)
		return false;
	switch (tx->stage) {
	case V90_PHASE3_RETURN_TX_SILENCE:
		return tx->s_symbols == 0 && tx->s_bar_symbols == 0 &&
		       !tx->finish_s_requested;
	case V90_PHASE3_RETURN_TX_S:
		return tx->s_bar_symbols == 0 &&
		       (!tx->finish_s_requested ||
			tx->s_symbols < V34_PHASE3_S_SYMBOLS ||
			(tx->s_symbols & 1u) != 0u);
	case V90_PHASE3_RETURN_TX_S_BAR:
		return tx->s_symbols >= V34_PHASE3_S_SYMBOLS &&
		       (tx->s_symbols & 1u) == 0u &&
		       tx->s_bar_symbols < V34_PHASE3_S_BAR_SYMBOLS &&
		       tx->finish_s_requested;
	case V90_PHASE3_RETURN_TX_COMPLETE:
		return tx->s_symbols >= V34_PHASE3_S_SYMBOLS &&
		       (tx->s_symbols & 1u) == 0u &&
		       tx->s_bar_symbols == V34_PHASE3_S_BAR_SYMBOLS &&
		       tx->finish_s_requested;
	}
	return false;
}

bool v90_phase3_return_tx_is_complete(
	const struct v90_phase3_return_tx *tx)
{
	return v90_phase3_return_tx_is_valid(tx) &&
	       tx->stage == V90_PHASE3_RETURN_TX_COMPLETE;
}

enum v90_phase3_return_tx_result v90_phase3_return_tx_init(
	struct v90_phase3_return_tx *tx)
{
	struct v90_phase3_return_tx initialized = {
		.stage = V90_PHASE3_RETURN_TX_SILENCE,
	};

	if (tx == NULL || !v90_phase3_return_tx_is_valid(&initialized))
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	*tx = initialized;
	return V90_PHASE3_RETURN_TX_OK;
}

enum v90_phase3_return_tx_result v90_phase3_return_tx_start_s(
	struct v90_phase3_return_tx *tx)
{
	if (!v90_phase3_return_tx_is_valid(tx))
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	if (tx->stage != V90_PHASE3_RETURN_TX_SILENCE)
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	tx->stage = V90_PHASE3_RETURN_TX_S;
	return V90_PHASE3_RETURN_TX_OK;
}

enum v90_phase3_return_tx_result v90_phase3_return_tx_finish_s(
	struct v90_phase3_return_tx *tx)
{
	if (!v90_phase3_return_tx_is_valid(tx))
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	if (tx->stage != V90_PHASE3_RETURN_TX_S)
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	if (tx->finish_s_requested)
		return V90_PHASE3_RETURN_TX_OK;
	tx->finish_s_requested = true;
	if (tx->s_symbols >= V34_PHASE3_S_SYMBOLS &&
	    (tx->s_symbols & 1u) == 0u)
		tx->stage = V90_PHASE3_RETURN_TX_S_BAR;
	return V90_PHASE3_RETURN_TX_OK;
}

enum v90_phase3_return_tx_result v90_phase3_return_tx_next(
	struct v90_phase3_return_tx *tx, struct qam_point *point)
{
	struct qam_point generated = { 0 };

	if (!v90_phase3_return_tx_is_valid(tx) || point == NULL ||
	    memory_regions_overlap(tx, sizeof(*tx), point, sizeof(*point)))
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	if (tx->stage == V90_PHASE3_RETURN_TX_COMPLETE)
		return V90_PHASE3_RETURN_TX_DONE;
	switch (tx->stage) {
	case V90_PHASE3_RETURN_TX_SILENCE:
		if (tx->silence_symbols == UINT32_MAX)
			return V90_PHASE3_RETURN_TX_OUT_OF_RANGE;
		tx->silence_symbols++;
		break;
	case V90_PHASE3_RETURN_TX_S:
		if (tx->s_symbols == UINT32_MAX)
			return V90_PHASE3_RETURN_TX_OUT_OF_RANGE;
		generated = v34_rotated_point_zero(
			(int)(tx->s_symbols & 1u));
		tx->s_symbols++;
		if (tx->finish_s_requested &&
		    tx->s_symbols >= V34_PHASE3_S_SYMBOLS &&
		    (tx->s_symbols & 1u) == 0u)
			tx->stage = V90_PHASE3_RETURN_TX_S_BAR;
		break;
	case V90_PHASE3_RETURN_TX_S_BAR:
		generated = v34_rotated_point_zero(
			(int)(2u + (tx->s_bar_symbols & 1u)));
		tx->s_bar_symbols++;
		if (tx->s_bar_symbols == V34_PHASE3_S_BAR_SYMBOLS)
			tx->stage = V90_PHASE3_RETURN_TX_COMPLETE;
		break;
	case V90_PHASE3_RETURN_TX_COMPLETE:
		return V90_PHASE3_RETURN_TX_DONE;
	default:
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	}
	*point = generated;
	return V90_PHASE3_RETURN_TX_OK;
}

const char *v90_phase3_return_tx_result_name(
	enum v90_phase3_return_tx_result result)
{
	switch (result) {
	case V90_PHASE3_RETURN_TX_OK:
		return "ok";
	case V90_PHASE3_RETURN_TX_DONE:
		return "done";
	case V90_PHASE3_RETURN_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_RETURN_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_RETURN_TX_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
