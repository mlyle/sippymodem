// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_tx.h"

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
			tx->s_symbols < V34_PHASE3_S_SYMBOLS);
	case V90_PHASE3_RETURN_TX_S_BAR:
		return tx->s_symbols >= V34_PHASE3_S_SYMBOLS &&
		       tx->s_bar_symbols < V34_PHASE3_S_BAR_SYMBOLS &&
		       tx->finish_s_requested;
	case V90_PHASE3_RETURN_TX_COMPLETE:
		return tx->s_symbols >= V34_PHASE3_S_SYMBOLS &&
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
	struct v90_phase3_return_tx next;

	if (!v90_phase3_return_tx_is_valid(tx))
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	if (tx->stage != V90_PHASE3_RETURN_TX_SILENCE)
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	next = *tx;
	next.stage = V90_PHASE3_RETURN_TX_S;
	if (!v90_phase3_return_tx_is_valid(&next))
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	*tx = next;
	return V90_PHASE3_RETURN_TX_OK;
}

enum v90_phase3_return_tx_result v90_phase3_return_tx_finish_s(
	struct v90_phase3_return_tx *tx)
{
	struct v90_phase3_return_tx next;

	if (!v90_phase3_return_tx_is_valid(tx))
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	if (tx->stage != V90_PHASE3_RETURN_TX_S)
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	if (tx->finish_s_requested)
		return V90_PHASE3_RETURN_TX_OK;
	next = *tx;
	next.finish_s_requested = true;
	if (next.s_symbols >= V34_PHASE3_S_SYMBOLS)
		next.stage = V90_PHASE3_RETURN_TX_S_BAR;
	if (!v90_phase3_return_tx_is_valid(&next))
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	*tx = next;
	return V90_PHASE3_RETURN_TX_OK;
}

enum v90_phase3_return_tx_result v90_phase3_return_tx_next(
	struct v90_phase3_return_tx *tx, struct qam_point *point)
{
	struct v90_phase3_return_tx next;
	struct qam_point generated = { 0 };

	if (!v90_phase3_return_tx_is_valid(tx) || point == NULL)
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	if (tx->stage == V90_PHASE3_RETURN_TX_COMPLETE)
		return V90_PHASE3_RETURN_TX_DONE;
	next = *tx;
	switch (next.stage) {
	case V90_PHASE3_RETURN_TX_SILENCE:
		if (next.silence_symbols == UINT32_MAX)
			return V90_PHASE3_RETURN_TX_OUT_OF_RANGE;
		next.silence_symbols++;
		break;
	case V90_PHASE3_RETURN_TX_S:
		if (next.s_symbols == UINT32_MAX)
			return V90_PHASE3_RETURN_TX_OUT_OF_RANGE;
		generated = v34_rotated_point_zero(
			(int)(next.s_symbols & 1u));
		next.s_symbols++;
		if (next.finish_s_requested &&
		    next.s_symbols >= V34_PHASE3_S_SYMBOLS)
			next.stage = V90_PHASE3_RETURN_TX_S_BAR;
		break;
	case V90_PHASE3_RETURN_TX_S_BAR:
		generated = v34_rotated_point_zero(
			(int)(2u + (next.s_bar_symbols & 1u)));
		next.s_bar_symbols++;
		if (next.s_bar_symbols == V34_PHASE3_S_BAR_SYMBOLS)
			next.stage = V90_PHASE3_RETURN_TX_COMPLETE;
		break;
	case V90_PHASE3_RETURN_TX_COMPLETE:
		return V90_PHASE3_RETURN_TX_DONE;
	default:
		return V90_PHASE3_RETURN_TX_INVALID_ARGUMENT;
	}
	if (!v90_phase3_return_tx_is_valid(&next))
		return V90_PHASE3_RETURN_TX_INVALID_STATE;
	*tx = next;
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
