// SPDX-License-Identifier: BSD-3-Clause

#include "v92_b1u_tx.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

bool v92_b1u_tx_is_valid(const struct v92_b1u_tx *tx)
{
	return tx != NULL && v92_pcm_frame_tx_is_valid(&tx->frame_tx) &&
	       tx->emitted_symbols <= V92_B1U_SYMBOL_COUNT &&
	       tx->upstream_level > 0;
}

bool v92_b1u_tx_is_complete(const struct v92_b1u_tx *tx)
{
	return v92_b1u_tx_is_valid(tx) && !tx->failed &&
	       tx->emitted_symbols == V92_B1U_SYMBOL_COUNT;
}

bool v92_b1u_tx_has_failed(const struct v92_b1u_tx *tx)
{
	return v92_b1u_tx_is_valid(tx) && tx->failed;
}

enum v92_b1u_tx_result v92_b1u_tx_init(
	struct v92_b1u_tx *tx, const struct v92_pcm_mapping *mapping,
	int16_t upstream_level)
{
	struct v92_b1u_tx initialized = { 0 };

	if (tx == NULL || mapping == NULL)
		return V92_B1U_TX_INVALID_ARGUMENT;
	if (upstream_level <= 0 ||
	    v92_pcm_frame_tx_init(&initialized.frame_tx, mapping) !=
		    V92_PCM_FRAME_OK)
		return V92_B1U_TX_INVALID_PARAMETERS;
	initialized.upstream_level = upstream_level;
	if (!v92_b1u_tx_is_valid(&initialized))
		return V92_B1U_TX_INVALID_STATE;
	*tx = initialized;
	return V92_B1U_TX_OK;
}

static enum v92_b1u_tx_result fail(struct v92_b1u_tx *tx)
{
	tx->failed = true;
	return V92_B1U_TX_NUMERIC_FAILURE;
}

enum v92_b1u_tx_result v92_b1u_tx_next(
	struct v92_b1u_tx *tx, int16_t *sample)
{
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	size_t symbol;
	int16_t generated;

	if (tx == NULL || sample == NULL ||
	    memory_regions_overlap(tx, sizeof(*tx), sample, sizeof(*sample)))
		return V92_B1U_TX_INVALID_ARGUMENT;
	if (!v92_b1u_tx_is_valid(tx))
		return V92_B1U_TX_INVALID_STATE;
	if (tx->failed)
		return V92_B1U_TX_FAILED;
	if (tx->emitted_symbols == V92_B1U_SYMBOL_COUNT)
		return V92_B1U_TX_COMPLETE;
	symbol = tx->emitted_symbols % V92_PCM_MAPPING_FRAME_SYMBOLS;
	if (symbol == 0u) {
		memset(ones, 1, tx->frame_tx.mapping.data_bits_per_frame);
		if (v92_pcm_frame_encode(
			    &tx->frame_tx, ones,
			    tx->frame_tx.mapping.data_bits_per_frame,
			    &tx->frame) != V92_PCM_FRAME_OK)
			return fail(tx);
	}
	if (v92_pcm_frame_scale(
		    &tx->frame_tx.mapping, tx->upstream_level,
		    tx->frame.v[symbol], &generated) != V92_PCM_FRAME_OK)
		return fail(tx);
	tx->emitted_symbols++;
	if (!v92_b1u_tx_is_valid(tx))
		return fail(tx);
	*sample = generated;
	return V92_B1U_TX_OK;
}

enum v92_b1u_tx_result v92_b1u_tx_handoff(
	const struct v92_b1u_tx *tx,
	struct v92_pcm_frame_tx *frame_tx)
{
	if (tx == NULL || frame_tx == NULL ||
	    memory_regions_overlap(
		    tx, sizeof(*tx), frame_tx, sizeof(*frame_tx)))
		return V92_B1U_TX_INVALID_ARGUMENT;
	if (!v92_b1u_tx_is_complete(tx))
		return V92_B1U_TX_INVALID_STATE;
	*frame_tx = tx->frame_tx;
	return V92_B1U_TX_OK;
}

const char *v92_b1u_tx_result_name(enum v92_b1u_tx_result result)
{
	switch (result) {
	case V92_B1U_TX_OK:
		return "ok";
	case V92_B1U_TX_COMPLETE:
		return "complete";
	case V92_B1U_TX_FAILED:
		return "failed";
	case V92_B1U_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_B1U_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_B1U_TX_INVALID_STATE:
		return "invalid state";
	case V92_B1U_TX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
