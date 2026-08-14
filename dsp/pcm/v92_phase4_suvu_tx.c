// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_suvu_tx.h"

#include "packed_bits.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool frame_is_valid(const struct v92_phase4_suvu_tx *tx)
{
	struct v92_suvu_parameters parameters;
	uint8_t canonical[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t bit_count = 0;

	return v92_suvu_decode(
		       tx->frame, sizeof(tx->frame), V92_SUVU_FRAME_BITS,
		       tx->modulator.constellation, &parameters) == V92_SUVU_OK &&
	       v92_suvu_encode(
		       &parameters, tx->modulator.constellation, canonical,
		       sizeof(canonical), &bit_count) == V92_SUVU_OK &&
	       bit_count == V92_SUVU_FRAME_BITS &&
	       memcmp(tx->frame, canonical, sizeof(canonical)) == 0;
}

bool v92_phase4_suvu_tx_is_valid(
	const struct v92_phase4_suvu_tx *tx)
{
	unsigned int bits;

	if (tx == NULL ||
	    !v92_phase4_modulator_is_valid(&tx->modulator))
		return false;
	bits = v92_phase4_modulator_bits_per_symbol(
		tx->modulator.constellation);
	return bits != 0u && tx->bit_index <= V92_SUVU_FRAME_BITS &&
	       tx->bit_index % bits == 0u && frame_is_valid(tx);
}

bool v92_phase4_suvu_tx_record_is_complete(
	const struct v92_phase4_suvu_tx *tx)
{
	return v92_phase4_suvu_tx_is_valid(tx) &&
	       tx->bit_index == V92_SUVU_FRAME_BITS;
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
	enum v92_phase4_suvu_tx_result result;

	if (tx == NULL || completed_trn2u == NULL || parameters == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_pcm_phase4_trn2u_is_valid(completed_trn2u) ||
	    !v92_pcm_phase4_trn2u_can_handoff(completed_trn2u))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	initialized.modulator = completed_trn2u->modulator;
	result = encode_record(
		parameters, initialized.modulator.constellation,
		initialized.frame);
	if (result != V92_PHASE4_SUVU_TX_OK)
		return result;
	if (!v92_phase4_suvu_tx_is_valid(&initialized))
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V92_PHASE4_SUVU_TX_OK;
}

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_start_next_record(
	struct v92_phase4_suvu_tx *tx,
	const struct v92_suvu_parameters *parameters)
{
	struct v92_phase4_suvu_tx next;
	enum v92_phase4_suvu_tx_result result;

	if (tx == NULL || parameters == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	if (!v92_phase4_suvu_tx_record_is_complete(tx))
		return V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS;
	next = *tx;
	result = encode_record(
		parameters, next.modulator.constellation, next.frame);
	if (result != V92_PHASE4_SUVU_TX_OK)
		return result;
	next.bit_index = 0;
	if (!v92_phase4_suvu_tx_is_valid(&next))
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PHASE4_SUVU_TX_OK;
}

static uint8_t input_group(
	const struct v92_phase4_suvu_tx *tx, unsigned int bits)
{
	uint8_t group = 0;

	for (unsigned int bit = 0; bit < bits; ++bit) {
		group |= (uint8_t)(
			packed_msb_bit(tx->frame, tx->bit_index + bit) << bit);
	}
	return group;
}

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_next(
	struct v92_phase4_suvu_tx *tx, int16_t *sample)
{
	struct v92_phase4_suvu_tx next;
	unsigned int bits;
	int16_t generated;

	if (tx == NULL || sample == NULL)
		return V92_PHASE4_SUVU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_suvu_tx_is_valid(tx))
		return V92_PHASE4_SUVU_TX_INVALID_STATE;
	if (v92_phase4_suvu_tx_record_is_complete(tx))
		return V92_PHASE4_SUVU_TX_RECORD_COMPLETE;
	next = *tx;
	bits = v92_phase4_modulator_bits_per_symbol(
		next.modulator.constellation);
	if (v92_phase4_modulator_next(
		    &next.modulator, input_group(&next, bits), &generated) !=
	    V92_PHASE4_MODULATOR_OK)
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	next.bit_index += bits;
	if (!v92_phase4_suvu_tx_is_valid(&next))
		return V92_PHASE4_SUVU_TX_INTERNAL_ERROR;
	*tx = next;
	*sample = generated;
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
	*modulator = tx->modulator;
	return V92_PHASE4_SUVU_TX_OK;
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
