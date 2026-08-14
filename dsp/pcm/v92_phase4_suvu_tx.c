// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_suvu_tx.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool frame_is_valid(const struct v92_phase4_suvu_tx *tx)
{
	struct v92_suvu_parameters parameters;
	uint8_t canonical[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t bit_count = 0;

	return v92_suvu_decode(
		       tx->record.frame, V92_SUVU_FRAME_BYTES,
		       V92_SUVU_FRAME_BITS,
		       tx->record.modulator.constellation,
		       &parameters) == V92_SUVU_OK &&
	       v92_suvu_encode(
		       &parameters, tx->record.modulator.constellation, canonical,
		       sizeof(canonical), &bit_count) == V92_SUVU_OK &&
	       bit_count == V92_SUVU_FRAME_BITS &&
	       memcmp(
		       tx->record.frame, canonical,
		       sizeof(canonical)) == 0;
}

bool v92_phase4_suvu_tx_is_valid(
	const struct v92_phase4_suvu_tx *tx)
{
	return tx != NULL &&
	       v92_phase4_record_tx_is_valid(&tx->record) &&
	       tx->record.frame_bit_count == V92_SUVU_FRAME_BITS &&
	       frame_is_valid(tx);
}

bool v92_phase4_suvu_tx_record_is_complete(
	const struct v92_phase4_suvu_tx *tx)
{
	return v92_phase4_suvu_tx_is_valid(tx) &&
	       v92_phase4_record_tx_is_complete(&tx->record);
}

static enum v92_phase4_suvu_tx_result encode_record(
	const struct v92_suvu_parameters *parameters,
	enum v92_constellation_size constellation,
	uint8_t frame[V92_SUVU_FRAME_BYTES])
{
	uint8_t encoded[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t bit_count = 0;
	enum v92_suvu_result result;

	if (parameters == NULL || frame == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_suvu_parameters_are_valid(parameters))
		return V92_PHASE4_SUVU_TX_INVALID_PARAMETERS;
	result = v92_suvu_encode(
		parameters, constellation, encoded, sizeof(encoded),
		&bit_count);
	if (result == V92_SUVU_INVALID_PARAMETERS)
		return V92_PHASE4_SUVU_TX_INVALID_PARAMETERS;
	if (result != V92_SUVU_OK || bit_count != V92_SUVU_FRAME_BITS)
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	memcpy(frame, encoded, sizeof(encoded));
	return V92_PHASE4_SUVU_TX_OK;
}

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_init(
	struct v92_phase4_suvu_tx *tx,
	const struct v92_pcm_phase4_trn2u *completed_trn2u,
	const struct v92_suvu_parameters *parameters)
{
	struct v92_phase4_suvu_tx initialized = { 0 };
	uint8_t frame[V92_SUVU_FRAME_BYTES];
	enum v92_phase4_suvu_tx_result result;

	if (tx == NULL || completed_trn2u == NULL || parameters == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_pcm_phase4_trn2u_is_valid(completed_trn2u) ||
	    !v92_pcm_phase4_trn2u_can_handoff(completed_trn2u))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	result = encode_record(
		parameters, completed_trn2u->modulator.constellation, frame);
	if (result != V92_PHASE4_SUVU_TX_OK)
		return result;
	if (v92_phase4_record_tx_init(
		    &initialized.record, &completed_trn2u->modulator,
		    frame, sizeof(frame), V92_SUVU_FRAME_BITS) !=
	    V92_PHASE4_RECORD_TX_OK)
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	if (!v92_phase4_suvu_tx_is_valid(&initialized))
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V92_PHASE4_SUVU_TX_OK;
}

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_start_next_record(
	struct v92_phase4_suvu_tx *tx,
	const struct v92_suvu_parameters *parameters)
{
	uint8_t frame[V92_SUVU_FRAME_BYTES];
	enum v92_phase4_suvu_tx_result result;

	if (tx == NULL || parameters == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	if (!v92_phase4_suvu_tx_record_is_complete(tx))
		return V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS;
	result = encode_record(
		parameters, tx->record.modulator.constellation, frame);
	if (result != V92_PHASE4_SUVU_TX_OK)
		return result;
	if (v92_phase4_record_tx_start_next(
		    &tx->record, frame, sizeof(frame),
		    V92_SUVU_FRAME_BITS) != V92_PHASE4_RECORD_TX_OK ||
	    !v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	return V92_PHASE4_SUVU_TX_OK;
}

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_next(
	struct v92_phase4_suvu_tx *tx, int16_t *sample)
{
	enum v92_phase4_record_tx_result result;

	if (tx == NULL || sample == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	if (v92_phase4_suvu_tx_record_is_complete(tx))
		return V92_PHASE4_SUVU_TX_RECORD_COMPLETE;
	result = v92_phase4_record_tx_next(&tx->record, sample);
	if (result == V92_PHASE4_RECORD_TX_COMPLETE)
		return V92_PHASE4_SUVU_TX_RECORD_COMPLETE;
	if (result != V92_PHASE4_RECORD_TX_OK ||
	    !v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	return V92_PHASE4_SUVU_TX_OK;
}

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_handoff(
	const struct v92_phase4_suvu_tx *tx,
	struct v92_phase4_modulator *modulator)
{
	if (tx == NULL || modulator == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	if (!v92_phase4_suvu_tx_record_is_complete(tx))
		return V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS;
	return v92_phase4_record_tx_handoff(&tx->record, modulator) ==
		       V92_PHASE4_RECORD_TX_OK ?
		V92_PHASE4_SUVU_TX_OK : V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
}

const char *v92_phase4_suvu_tx_result_name(
	enum v92_phase4_suvu_tx_result result)
{
	switch (result) {
	case V92_PHASE4_SUVU_TX_OK:
		return "ok";
	case V92_PHASE4_SUVU_TX_RECORD_COMPLETE:
		return "record complete";
	case V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS:
		return "record in progress";
	case V92_PHASE4_SUVU_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_SUVU_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PHASE4_SUVU_TX_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_SUVU_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
