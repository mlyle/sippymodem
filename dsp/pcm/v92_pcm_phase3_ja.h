// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_JA_H
#define V92_PCM_PHASE3_JA_H

#include "v92_pcm_dil.h"
#include "v92_pcm_phase3_trn1u.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE3_JA_PREAMBLE_BITS = 24,
	V92_PCM_PHASE3_JA_MIN_SYMBOLS =
		V92_PCM_PHASE3_JA_PREAMBLE_BITS +
		V92_PCM_DIL_ZERO_DESCRIPTOR_BITS,
	V92_PCM_PHASE3_JA_ALIGNMENT = 12,
};

enum v92_pcm_phase3_ja_result {
	V92_PCM_PHASE3_JA_OK,
	V92_PCM_PHASE3_JA_INVALID_ARGUMENT,
	V92_PCM_PHASE3_JA_INVALID_STATE,
};

/* Ja continues the TRN1u GPA scrambler and sign, then emits 24 binary ones
 * followed by the selected DIL descriptor repeatedly. The owning Phase 3
 * controller detects the peer response and chooses an aligned endpoint. */
struct v92_pcm_phase3_ja_tx {
	struct v34_scrambler scrambler;
	uint64_t emitted_symbols;
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	enum g711_law law;
	uint8_t differential_memory;
	uint8_t lu_ucode;
};

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_init(
	struct v92_pcm_phase3_ja_tx *tx,
	const struct v92_pcm_phase3_trn1u *trn1u,
	uint32_t upstream_rate_mask);
bool v92_pcm_phase3_ja_tx_is_valid(
	const struct v92_pcm_phase3_ja_tx *tx);
bool v92_pcm_phase3_ja_tx_can_stop(
	const struct v92_pcm_phase3_ja_tx *tx);

enum v92_pcm_phase3_ja_result v92_pcm_phase3_ja_tx_next(
	struct v92_pcm_phase3_ja_tx *tx, struct v90_pcm_symbol *symbol);

const char *v92_pcm_phase3_ja_result_name(
	enum v92_pcm_phase3_ja_result result);

#endif /* V92_PCM_PHASE3_JA_H */
