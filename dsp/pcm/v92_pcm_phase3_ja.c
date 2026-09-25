// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_ja.h"

#include "byte_fingerprint.h"
#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The encoder validates the complete request once. Guard the owned bytes
 * and their selection without decoding the same DIL at every 8 kHz sample. */
static uint32_t descriptor_fingerprint(
	const struct v92_pcm_phase3_ja_tx *tx)
{
	uint32_t fingerprint = byte_fingerprint(
		tx->descriptor, (tx->descriptor_bits + 7u) / 8u);

	fingerprint = byte_fingerprint_mix_u32(
		fingerprint, tx->descriptor_bits);
	fingerprint = byte_fingerprint_mix_u32(
		fingerprint, tx->upstream_rate_mask);
	return byte_fingerprint_mix_u32(fingerprint, tx->dil_segments);
}

bool v92_pcm_phase3_ja_tx_is_valid(
	const struct v92_pcm_phase3_ja_tx *tx)
{
	return tx != NULL && g711_law_is_valid(tx->law) &&
	       tx->lu_ucode > 0u && tx->lu_ucode <= 127u &&
	       pcm_binary_sign_is_valid(&tx->coding) &&
	       tx->coding.scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	       tx->descriptor_bits >= V92_PCM_DIL_ZERO_DESCRIPTOR_BITS &&
	       tx->descriptor_bits <= V92_PCM_DIL_MAX_DESCRIPTOR_BITS &&
	       tx->descriptor_bits % V92_PCM_PHASE3_JA_ALIGNMENT == 0u &&
	       v92_pcm_dil_rate_mask_is_valid(tx->upstream_rate_mask) &&
	       tx->descriptor_fingerprint == descriptor_fingerprint(tx);
}

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_init_dil(
	struct v92_pcm_phase3_ja_tx *tx,
	const struct v92_pcm_phase3_trn1u *trn1u,
	const struct v92_pcm_dil_parameters *parameters)
{
	struct v92_pcm_phase3_ja_tx initialized = { 0 };

	if (tx == NULL || !v92_pcm_phase3_trn1u_can_handoff(trn1u))
		return V92_PCM_PHASE3_JA_INVALID_ARGUMENT;
	initialized.law = trn1u->law;
	initialized.lu_ucode = trn1u->lu_ucode;
	if (pcm_binary_sign_init_from_scrambler(
		    &initialized.coding, &trn1u->scrambler,
		    (uint8_t)(trn1u->scrambler.history & 1u)) !=
		    PCM_BINARY_SIGN_OK ||
	    v92_pcm_dil_encode(
		    parameters, initialized.descriptor,
		    sizeof(initialized.descriptor),
		    &initialized.descriptor_bits) != V92_PCM_DIL_OK)
		return V92_PCM_PHASE3_JA_INVALID_ARGUMENT;
	initialized.upstream_rate_mask = parameters->upstream_rate_mask;
	initialized.dil_segments = parameters->downstream.segment_count;
	initialized.descriptor_fingerprint = descriptor_fingerprint(&initialized);
	if (!v92_pcm_phase3_ja_tx_is_valid(&initialized))
		return V92_PCM_PHASE3_JA_INVALID_ARGUMENT;
	*tx = initialized;
	return V92_PCM_PHASE3_JA_OK;
}

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_init(
	struct v92_pcm_phase3_ja_tx *tx,
	const struct v92_pcm_phase3_trn1u *trn1u,
	uint32_t upstream_rate_mask)
{
	const struct v92_pcm_dil_parameters parameters = {
		.downstream = { .sign_pattern_bits = 1, .training_pattern_bits = 1 },
		.upstream_rate_mask = upstream_rate_mask,
	};

	return v92_pcm_phase3_ja_tx_init_dil(tx, trn1u, &parameters);
}

bool v92_pcm_phase3_ja_tx_can_stop(
	const struct v92_pcm_phase3_ja_tx *tx)
{
	return v92_pcm_phase3_ja_tx_is_valid(tx) &&
	       tx->emitted_symbols >=
		       V92_PCM_PHASE3_JA_PREAMBLE_BITS + tx->descriptor_bits &&
	       tx->emitted_symbols % V92_PCM_PHASE3_JA_ALIGNMENT == 0u;
}

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_next(
	struct v92_pcm_phase3_ja_tx *tx, struct v90_pcm_symbol *symbol)
{
	struct v92_pcm_phase3_ja_tx next;
	struct v90_pcm_symbol generated;
	uint64_t descriptor_index;
	uint8_t input_bit;
	uint8_t sign;

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
			next.descriptor_bits;
		input_bit = packed_msb_bit(
			next.descriptor, (size_t)descriptor_index);
	}
	if (pcm_binary_sign_encode(&next.coding, input_bit, &sign) !=
	    PCM_BINARY_SIGN_OK)
		return V92_PCM_PHASE3_JA_INVALID_STATE;
	if (!v90_pcm_symbol_make(
		    &generated, next.law, next.lu_ucode,
		    sign == 0u))
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
