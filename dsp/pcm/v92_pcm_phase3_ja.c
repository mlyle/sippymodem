// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_ja.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static uint8_t descriptor_bit(
	const uint8_t *descriptor, size_t bit_index)
{
	return (uint8_t)((descriptor[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			  1u);
}

bool v92_pcm_phase3_ja_tx_is_valid(
	const struct v92_pcm_phase3_ja_tx *tx)
{
	uint32_t rate_mask;

	return tx != NULL && law_is_valid(tx->law) &&
	       tx->lu_ucode > 0u && tx->lu_ucode <= 127u &&
	       tx->differential_memory <= 1u &&
	       v34_scrambler_is_valid(&tx->scrambler) &&
	       tx->scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	       v92_pcm_dil_decode_zero(
		       tx->descriptor, sizeof(tx->descriptor), &rate_mask) ==
		       V92_PCM_DIL_OK;
}

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_init(
	struct v92_pcm_phase3_ja_tx *tx,
	const struct v92_pcm_phase3_trn1u *trn1u,
	uint32_t upstream_rate_mask)
{
	struct v92_pcm_phase3_ja_tx initialized = { 0 };

	if (tx == NULL || !v92_pcm_phase3_trn1u_can_handoff(trn1u) ||
	    !v92_pcm_dil_rate_mask_is_valid(upstream_rate_mask))
		return V92_PCM_PHASE3_JA_INVALID_ARGUMENT;
	initialized.scrambler = trn1u->scrambler;
	initialized.law = trn1u->law;
	initialized.lu_ucode = trn1u->lu_ucode;
	initialized.differential_memory =
		(uint8_t)(trn1u->scrambler.history & 1u);
	if (v92_pcm_dil_encode_zero(
		    upstream_rate_mask, initialized.descriptor,
		    sizeof(initialized.descriptor)) != V92_PCM_DIL_OK ||
	    !v92_pcm_phase3_ja_tx_is_valid(&initialized))
		return V92_PCM_PHASE3_JA_INVALID_ARGUMENT;
	*tx = initialized;
	return V92_PCM_PHASE3_JA_OK;
}

bool v92_pcm_phase3_ja_tx_can_stop(
	const struct v92_pcm_phase3_ja_tx *tx)
{
	return v92_pcm_phase3_ja_tx_is_valid(tx) &&
	       tx->emitted_symbols >= V92_PCM_PHASE3_JA_MIN_SYMBOLS &&
	       tx->emitted_symbols % V92_PCM_PHASE3_JA_ALIGNMENT == 0u;
}

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_next(
	struct v92_pcm_phase3_ja_tx *tx, struct v90_pcm_symbol *symbol)
{
	struct v92_pcm_phase3_ja_tx next;
	struct v90_pcm_symbol generated;
	uint64_t descriptor_index;
	uint8_t input_bit;
	uint8_t scrambled;

	if (tx == NULL || symbol == NULL)
		return V92_PCM_PHASE3_JA_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_ja_tx_is_valid(tx) ||
	    tx->emitted_symbols == UINT64_MAX)
		return V92_PCM_PHASE3_JA_INVALID_STATE;
	next = *tx;
	if (next.emitted_symbols < V92_PCM_PHASE3_JA_PREAMBLE_BITS) {
		input_bit = 1u;
	} else {
		descriptor_index =
			(next.emitted_symbols - V92_PCM_PHASE3_JA_PREAMBLE_BITS) %
			V92_PCM_DIL_ZERO_DESCRIPTOR_BITS;
		input_bit = descriptor_bit(
			next.descriptor, (size_t)descriptor_index);
	}
	if (v34_scramble_group(
		    &next.scrambler, input_bit, 1u, &scrambled) !=
	    V34_FRAMING_OK)
		return V92_PCM_PHASE3_JA_INVALID_STATE;
	next.differential_memory ^= scrambled;
	if (!v90_pcm_symbol_make(
		    &generated, next.law, next.lu_ucode,
		    next.differential_memory == 0u))
		return V92_PCM_PHASE3_JA_INVALID_STATE;
	next.emitted_symbols++;
	if (!v92_pcm_phase3_ja_tx_is_valid(&next))
		return V92_PCM_PHASE3_JA_INVALID_STATE;
	*tx = next;
	*symbol = generated;
	return V92_PCM_PHASE3_JA_OK;
}

const char *v92_pcm_phase3_ja_result_name(
	enum v92_pcm_phase3_ja_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_JA_OK:
		return "ok";
	case V92_PCM_PHASE3_JA_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_JA_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
