// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_initial_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool stage_is_valid(enum v92_pcm_phase3_initial_tx_stage stage)
{
	return stage >= V92_PCM_PHASE3_INITIAL_TX_OPENING &&
	       stage <= V92_PCM_PHASE3_INITIAL_TX_COMPLETE;
}

static bool trn1u_goal_is_valid(uint32_t symbols)
{
	return symbols >= V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
	       symbols % V92_PCM_PHASE3_TRN1U_ALIGNMENT == 0u;
}

static bool opening_matches(
	const struct v92_pcm_phase3_initial_tx *tx)
{
	const struct v92_pcm_phase3_opening *opening = &tx->sequence.opening;

	return v92_pcm_phase3_opening_is_valid(opening) &&
	       !v92_pcm_phase3_opening_is_complete(opening) &&
	       opening->law == tx->law && opening->lu_ucode == tx->lu_ucode &&
	       opening->md_length_276_symbol_units ==
		       tx->md_length_276_symbol_units;
}

static bool trn1u_matches(
	const struct v92_pcm_phase3_initial_tx *tx)
{
	const struct v92_pcm_phase3_trn1u *trn1u = &tx->sequence.trn1u;

	return v92_pcm_phase3_trn1u_is_valid(trn1u) &&
	       trn1u->law == tx->law && trn1u->lu_ucode == tx->lu_ucode &&
	       trn1u->emitted_symbols < tx->trn1u_goal_symbols;
}

static bool ja_matches(const struct v92_pcm_phase3_initial_tx *tx)
{
	const struct v92_pcm_phase3_ja_tx *ja = &tx->sequence.ja;
	uint32_t rate_mask;

	return v92_pcm_phase3_ja_tx_is_valid(ja) && ja->law == tx->law &&
	       ja->lu_ucode == tx->lu_ucode &&
	       v92_pcm_dil_decode_zero(
		       ja->descriptor, sizeof(ja->descriptor), &rate_mask) ==
		       V92_PCM_DIL_OK &&
	       rate_mask == tx->upstream_rate_mask;
}

bool v92_pcm_phase3_initial_tx_is_valid(
	const struct v92_pcm_phase3_initial_tx *tx)
{
	if (tx == NULL || !stage_is_valid(tx->stage) ||
	    !g711_law_is_valid(tx->law) || tx->lu_ucode == 0u ||
	    tx->lu_ucode > 127u ||
	    tx->md_length_276_symbol_units >
		    V92_PCM_INFO1A_MAX_MD_LENGTH_UNITS ||
	    !v92_pcm_dil_rate_mask_is_valid(tx->upstream_rate_mask) ||
	    !trn1u_goal_is_valid(tx->trn1u_goal_symbols))
		return false;
	switch (tx->stage) {
	case V92_PCM_PHASE3_INITIAL_TX_OPENING:
		return !tx->finish_requested && opening_matches(tx);
	case V92_PCM_PHASE3_INITIAL_TX_TRN1U:
		return !tx->finish_requested && trn1u_matches(tx);
	case V92_PCM_PHASE3_INITIAL_TX_JA:
		return ja_matches(tx) &&
		       !(tx->finish_requested &&
			 v92_pcm_phase3_ja_tx_can_stop(&tx->sequence.ja));
	case V92_PCM_PHASE3_INITIAL_TX_COMPLETE:
		return tx->finish_requested && ja_matches(tx) &&
		       v92_pcm_phase3_ja_tx_can_stop(&tx->sequence.ja);
	}
	return false;
}

bool v92_pcm_phase3_initial_tx_is_complete(
	const struct v92_pcm_phase3_initial_tx *tx)
{
	return v92_pcm_phase3_initial_tx_is_valid(tx) &&
	       tx->stage == V92_PCM_PHASE3_INITIAL_TX_COMPLETE;
}

enum v92_pcm_phase3_initial_tx_result v92_pcm_phase3_initial_tx_init(
	struct v92_pcm_phase3_initial_tx *tx, enum g711_law law,
	uint8_t lu_ucode, const struct v92_pcm_info1a *info1a,
	uint32_t upstream_rate_mask, uint32_t trn1u_goal_symbols)
{
	struct v92_pcm_phase3_initial_tx initialized = {
		.trn1u_goal_symbols = trn1u_goal_symbols,
		.upstream_rate_mask = upstream_rate_mask,
		.stage = V92_PCM_PHASE3_INITIAL_TX_OPENING,
		.law = law,
		.lu_ucode = lu_ucode,
	};

	if (tx == NULL || !v92_pcm_info1a_is_valid(info1a) ||
	    !v92_pcm_dil_rate_mask_is_valid(upstream_rate_mask) ||
	    !trn1u_goal_is_valid(trn1u_goal_symbols) ||
	    v92_pcm_phase3_opening_init(
		    &initialized.sequence.opening, law, lu_ucode, info1a) !=
		    V92_PCM_PHASE3_OPENING_OK)
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT;
	initialized.md_length_276_symbol_units =
		info1a->local_md_length_276_symbol_units;
	if (!v92_pcm_phase3_initial_tx_is_valid(&initialized))
		return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V92_PCM_PHASE3_INITIAL_TX_OK;
}

static enum v92_pcm_phase3_initial_tx_result next_opening(
	struct v92_pcm_phase3_initial_tx *tx,
	const struct v90_pcm_symbol *md_symbol,
	struct v90_pcm_symbol *symbol)
{
	enum v92_pcm_phase3_opening_result result =
		v92_pcm_phase3_opening_next(
			&tx->sequence.opening, md_symbol, symbol);

	if (result == V92_PCM_PHASE3_OPENING_NEED_MD_SYMBOL)
		return V92_PCM_PHASE3_INITIAL_TX_NEED_MD_SYMBOL;
	if (result == V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT)
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT;
	if (result != V92_PCM_PHASE3_OPENING_OK)
		return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
	if (!v92_pcm_phase3_opening_is_complete(&tx->sequence.opening))
		return V92_PCM_PHASE3_INITIAL_TX_OK;
	{
		struct v92_pcm_phase3_trn1u trn1u;

		if (v92_pcm_phase3_trn1u_init(
			    &trn1u, tx->law, tx->lu_ucode) !=
		    V92_PCM_PHASE3_TRN1U_OK)
			return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
		tx->sequence =
			(union v92_pcm_phase3_initial_tx_sequence){
				.trn1u = trn1u,
			};
	}
	tx->stage = V92_PCM_PHASE3_INITIAL_TX_TRN1U;
	return V92_PCM_PHASE3_INITIAL_TX_OK;
}

static enum v92_pcm_phase3_initial_tx_result next_trn1u(
	struct v92_pcm_phase3_initial_tx *tx,
	struct v90_pcm_symbol *symbol)
{
	if (v92_pcm_phase3_trn1u_next(&tx->sequence.trn1u, symbol) !=
	    V92_PCM_PHASE3_TRN1U_OK)
		return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
	if (tx->sequence.trn1u.emitted_symbols != tx->trn1u_goal_symbols)
		return V92_PCM_PHASE3_INITIAL_TX_OK;
	{
		struct v92_pcm_phase3_ja_tx ja;

		if (v92_pcm_phase3_ja_tx_init(
			    &ja, &tx->sequence.trn1u,
			    tx->upstream_rate_mask) != V92_PCM_PHASE3_JA_OK)
			return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
		tx->sequence =
			(union v92_pcm_phase3_initial_tx_sequence){ .ja = ja };
	}
	tx->stage = V92_PCM_PHASE3_INITIAL_TX_JA;
	return V92_PCM_PHASE3_INITIAL_TX_OK;
}

static enum v92_pcm_phase3_initial_tx_result next_ja(
	struct v92_pcm_phase3_initial_tx *tx,
	struct v90_pcm_symbol *symbol)
{
	if (v92_pcm_phase3_ja_tx_next(&tx->sequence.ja, symbol) !=
	    V92_PCM_PHASE3_JA_OK)
		return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
	if (tx->finish_requested &&
	    v92_pcm_phase3_ja_tx_can_stop(&tx->sequence.ja))
		tx->stage = V92_PCM_PHASE3_INITIAL_TX_COMPLETE;
	return V92_PCM_PHASE3_INITIAL_TX_OK;
}

static bool md_symbol_is_permitted(
	const struct v92_pcm_phase3_initial_tx *tx,
	const struct v90_pcm_symbol *md_symbol)
{
	return md_symbol == NULL ||
	       (tx->stage == V92_PCM_PHASE3_INITIAL_TX_OPENING &&
		tx->sequence.opening.stage == V92_PCM_PHASE3_OPENING_MD);
}

enum v92_pcm_phase3_initial_tx_result v92_pcm_phase3_initial_tx_next(
	struct v92_pcm_phase3_initial_tx *tx,
	const struct v90_pcm_symbol *md_symbol,
	struct v90_pcm_symbol *symbol)
{
	struct v92_pcm_phase3_initial_tx next;
	struct v90_pcm_symbol generated;
	enum v92_pcm_phase3_initial_tx_result result;

	if (tx == NULL || symbol == NULL)
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_initial_tx_is_valid(tx))
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE;
	if (!md_symbol_is_permitted(tx, md_symbol))
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT;
	if (tx->stage == V92_PCM_PHASE3_INITIAL_TX_COMPLETE)
		return V92_PCM_PHASE3_INITIAL_TX_DONE;
	next = *tx;
	switch (next.stage) {
	case V92_PCM_PHASE3_INITIAL_TX_OPENING:
		result = next_opening(&next, md_symbol, &generated);
		break;
	case V92_PCM_PHASE3_INITIAL_TX_TRN1U:
		result = next_trn1u(&next, &generated);
		break;
	case V92_PCM_PHASE3_INITIAL_TX_JA:
		result = next_ja(&next, &generated);
		break;
	case V92_PCM_PHASE3_INITIAL_TX_COMPLETE:
		return V92_PCM_PHASE3_INITIAL_TX_DONE;
	default:
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT;
	}
	if (result != V92_PCM_PHASE3_INITIAL_TX_OK)
		return result;
	if (!v92_pcm_phase3_initial_tx_is_valid(&next))
		return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
	*tx = next;
	*symbol = generated;
	return V92_PCM_PHASE3_INITIAL_TX_OK;
}

enum v92_pcm_phase3_initial_tx_result
v92_pcm_phase3_initial_tx_request_finish(
	struct v92_pcm_phase3_initial_tx *tx)
{
	struct v92_pcm_phase3_initial_tx next;

	if (tx == NULL)
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_initial_tx_is_valid(tx))
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE;
	if (tx->stage != V92_PCM_PHASE3_INITIAL_TX_JA)
		return V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE;
	next = *tx;
	next.finish_requested = true;
	if (v92_pcm_phase3_ja_tx_can_stop(&next.sequence.ja))
		next.stage = V92_PCM_PHASE3_INITIAL_TX_COMPLETE;
	if (!v92_pcm_phase3_initial_tx_is_valid(&next))
		return V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR;
	*tx = next;
	return V92_PCM_PHASE3_INITIAL_TX_OK;
}

const char *v92_pcm_phase3_initial_tx_stage_name(
	enum v92_pcm_phase3_initial_tx_stage stage)
{
	switch (stage) {
	case V92_PCM_PHASE3_INITIAL_TX_OPENING:
		return "opening";
	case V92_PCM_PHASE3_INITIAL_TX_TRN1U:
		return "TRN1u";
	case V92_PCM_PHASE3_INITIAL_TX_JA:
		return "Ja";
	case V92_PCM_PHASE3_INITIAL_TX_COMPLETE:
		return "complete";
	}
	return "unknown";
}

const char *v92_pcm_phase3_initial_tx_result_name(
	enum v92_pcm_phase3_initial_tx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_INITIAL_TX_OK:
		return "ok";
	case V92_PCM_PHASE3_INITIAL_TX_NEED_MD_SYMBOL:
		return "need MD symbol";
	case V92_PCM_PHASE3_INITIAL_TX_DONE:
		return "done";
	case V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
