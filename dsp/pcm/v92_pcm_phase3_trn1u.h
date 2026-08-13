// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_TRN1U_H
#define V92_PCM_PHASE3_TRN1U_H

#include "v34_framing.h"
#include "v92_pcm_phase3_opening.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS = 2040,
	V92_PCM_PHASE3_TRN1U_ALIGNMENT = 12,
};

enum v92_pcm_phase3_trn1u_result {
	V92_PCM_PHASE3_TRN1U_OK,
	V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT,
	V92_PCM_PHASE3_TRN1U_INVALID_STATE,
};

/* Generate a continuous raw two-point TRN1u segment. The owning Phase 3
 * controller chooses its aligned endpoint. The live GPA history is retained;
 * its bit zero is the final sign needed by the following Ja sequence. */
struct v92_pcm_phase3_trn1u {
	struct v34_scrambler scrambler;
	enum g711_law law;
	uint32_t emitted_symbols;
	uint8_t lu_ucode;
};

enum v92_pcm_phase3_trn1u_result v92_pcm_phase3_trn1u_init(
	struct v92_pcm_phase3_trn1u *trn1u,
	const struct v92_pcm_phase3_opening *opening);
bool v92_pcm_phase3_trn1u_is_valid(
	const struct v92_pcm_phase3_trn1u *trn1u);
bool v92_pcm_phase3_trn1u_can_handoff(
	const struct v92_pcm_phase3_trn1u *trn1u);

enum v92_pcm_phase3_trn1u_result v92_pcm_phase3_trn1u_next(
	struct v92_pcm_phase3_trn1u *trn1u,
	struct v90_pcm_symbol *symbol);

const char *v92_pcm_phase3_trn1u_result_name(
	enum v92_pcm_phase3_trn1u_result result);

#endif /* V92_PCM_PHASE3_TRN1U_H */
