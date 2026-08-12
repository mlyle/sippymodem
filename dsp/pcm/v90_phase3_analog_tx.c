// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_tx.h"

#include <stddef.h>

static bool prefix_is_valid(
	const struct v34_phase3_transmitter *prefix)
{
	return v34_phase3_transmitter_is_valid(prefix) &&
	       prefix->local_role == V34_ANSWERING_STATION &&
	       prefix->remote_constellation == V34_CONSTELLATION_4_POINT &&
	       prefix->stage <= V34_PHASE3_TRANSMITTER_TRN;
}

static bool ja_count_matches(const struct v90_phase3_analog_tx *tx)
{
	uint32_t position =
		tx->ja_symbols % V90_PHASE3_JA_DESCRIPTOR_SYMBOLS;

	return tx->sequence.ja.bit_index == position * 2u;
}

bool v90_phase3_analog_tx_is_valid(
	const struct v90_phase3_analog_tx *tx)
{
	if (tx == NULL)
		return false;
	switch (tx->stage) {
	case V90_PHASE3_ANALOG_TX_PREFIX:
		return tx->ja_symbols == 0 &&
		       prefix_is_valid(&tx->sequence.prefix);
	case V90_PHASE3_ANALOG_TX_JA:
		return v90_ja_tx_is_valid(&tx->sequence.ja) &&
		       ja_count_matches(tx);
	case V90_PHASE3_ANALOG_TX_COMPLETE:
		return tx->ja_symbols >= V90_PHASE3_JA_DESCRIPTOR_SYMBOLS &&
		       v90_ja_tx_is_valid(&tx->sequence.ja) &&
		       ja_count_matches(tx);
	}
	return false;
}

enum v90_phase3_analog_tx_result v90_phase3_analog_tx_init(
	struct v90_phase3_analog_tx *tx)
{
	struct v90_phase3_analog_tx initialized = {
		.stage = V90_PHASE3_ANALOG_TX_PREFIX,
	};

	if (tx == NULL ||
	    v34_phase3_transmitter_init(
		    &initialized.sequence.prefix,
		    V34_ANSWERING_STATION,
		    V34_CONSTELLATION_4_POINT) != V34_PHASE3_TRANSMITTER_OK ||
	    !v90_phase3_analog_tx_is_valid(&initialized))
		return V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT;
	*tx = initialized;
	return V90_PHASE3_ANALOG_TX_OK;
}

enum v90_phase3_analog_tx_result v90_phase3_analog_tx_next(
	struct v90_phase3_analog_tx *tx, struct qam_point *point)
{
	struct v90_phase3_analog_tx next;
	struct qam_point generated;

	if (!v90_phase3_analog_tx_is_valid(tx) || point == NULL)
		return V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT;
	if (tx->stage == V90_PHASE3_ANALOG_TX_COMPLETE)
		return V90_PHASE3_ANALOG_TX_DONE;
	next = *tx;
	if (next.stage == V90_PHASE3_ANALOG_TX_PREFIX) {
		if (v34_phase3_transmitter_next(
			    &next.sequence.prefix, &generated) !=
		    V34_PHASE3_TRANSMITTER_OK)
			return V90_PHASE3_ANALOG_TX_INTERNAL_ERROR;
	} else {
		uint8_t rotation;

		if (next.ja_symbols == UINT32_MAX ||
		    v90_ja_tx_next(
			    &next.sequence.ja, &generated, &rotation) != V90_JA_OK)
			return V90_PHASE3_ANALOG_TX_INTERNAL_ERROR;
		next.ja_symbols++;
	}
	if (!v90_phase3_analog_tx_is_valid(&next))
		return V90_PHASE3_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	*point = generated;
	return V90_PHASE3_ANALOG_TX_OK;
}

enum v90_phase3_analog_tx_result v90_phase3_analog_tx_start_ja(
	struct v90_phase3_analog_tx *tx)
{
	struct v90_phase3_analog_tx next;
	struct v34_phase3_trn boundary;
	struct v90_ja_tx ja;
	enum v34_phase3_transmitter_result boundary_result;

	if (!v90_phase3_analog_tx_is_valid(tx))
		return V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT;
	if (tx->stage != V90_PHASE3_ANALOG_TX_PREFIX)
		return V90_PHASE3_ANALOG_TX_INVALID_STATE;
	boundary_result = v34_phase3_transmitter_trn_boundary(
		&tx->sequence.prefix, &boundary);
	if (boundary_result == V34_PHASE3_TRANSMITTER_INVALID_STATE)
		return V90_PHASE3_ANALOG_TX_INVALID_STATE;
	if (boundary_result != V34_PHASE3_TRANSMITTER_OK ||
	    v90_ja_tx_init_from_trn(&ja, &boundary) != V90_JA_OK)
		return V90_PHASE3_ANALOG_TX_INTERNAL_ERROR;
	next = *tx;
	next.sequence = (union v90_phase3_analog_tx_sequence){ 0 };
	next.sequence.ja = ja;
	next.stage = V90_PHASE3_ANALOG_TX_JA;
	if (!v90_phase3_analog_tx_is_valid(&next))
		return V90_PHASE3_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V90_PHASE3_ANALOG_TX_OK;
}

enum v90_phase3_analog_tx_result v90_phase3_analog_tx_finish_ja(
	struct v90_phase3_analog_tx *tx)
{
	struct v90_phase3_analog_tx next;

	if (!v90_phase3_analog_tx_is_valid(tx))
		return V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT;
	if (tx->stage != V90_PHASE3_ANALOG_TX_JA ||
	    tx->ja_symbols < V90_PHASE3_JA_DESCRIPTOR_SYMBOLS)
		return V90_PHASE3_ANALOG_TX_INVALID_STATE;
	next = *tx;
	next.stage = V90_PHASE3_ANALOG_TX_COMPLETE;
	if (!v90_phase3_analog_tx_is_valid(&next))
		return V90_PHASE3_ANALOG_TX_INTERNAL_ERROR;
	*tx = next;
	return V90_PHASE3_ANALOG_TX_OK;
}

const char *v90_phase3_analog_tx_result_name(
	enum v90_phase3_analog_tx_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_TX_OK:
		return "ok";
	case V90_PHASE3_ANALOG_TX_DONE:
		return "done";
	case V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_ANALOG_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_ANALOG_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
