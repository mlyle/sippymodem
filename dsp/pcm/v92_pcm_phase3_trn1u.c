// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_trn1u.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool v92_pcm_phase3_trn1u_is_valid(
	const struct v92_pcm_phase3_trn1u *trn1u)
{
	return trn1u != NULL && g711_law_is_valid(trn1u->law) &&
	       trn1u->lu_ucode > 0u && trn1u->lu_ucode <= 127u &&
	       v34_scrambler_is_valid(&trn1u->scrambler) &&
	       trn1u->scrambler.polynomial == V34_POLYNOMIAL_GPA;
}

enum v92_pcm_phase3_trn1u_result v92_pcm_phase3_trn1u_init(
	struct v92_pcm_phase3_trn1u *trn1u,
	enum g711_law law, uint8_t lu_ucode)
{
	struct v92_pcm_phase3_trn1u initialized = { 0 };

	if (trn1u == NULL || !g711_law_is_valid(law) ||
	    lu_ucode == 0u || lu_ucode > 127u ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK)
		return V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT;
	initialized.law = law;
	initialized.lu_ucode = lu_ucode;
	if (!v92_pcm_phase3_trn1u_is_valid(&initialized))
		return V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT;
	*trn1u = initialized;
	return V92_PCM_PHASE3_TRN1U_OK;
}

bool v92_pcm_phase3_trn1u_can_handoff(
	const struct v92_pcm_phase3_trn1u *trn1u)
{
	return v92_pcm_phase3_trn1u_is_valid(trn1u) &&
	       trn1u->emitted_symbols >= V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
	       trn1u->emitted_symbols % V92_PCM_PHASE3_TRN1U_ALIGNMENT == 0u;
}

enum v92_pcm_phase3_trn1u_result v92_pcm_phase3_trn1u_next(
	struct v92_pcm_phase3_trn1u *trn1u,
	struct v90_pcm_symbol *symbol)
{
	struct v92_pcm_phase3_trn1u next;
	struct v90_pcm_symbol generated;
	uint8_t scrambled;

	if (trn1u == NULL || symbol == NULL)
		return V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_trn1u_is_valid(trn1u) ||
	    trn1u->emitted_symbols == UINT32_MAX)
		return V92_PCM_PHASE3_TRN1U_INVALID_STATE;
	next = *trn1u;
	if (v34_scramble_group(&next.scrambler, 1u, 1u, &scrambled) !=
		    V34_FRAMING_OK ||
	    !v90_pcm_symbol_make(
		    &generated, next.law, next.lu_ucode, scrambled == 0u))
		return V92_PCM_PHASE3_TRN1U_INVALID_STATE;
	next.emitted_symbols++;
	if (!v92_pcm_phase3_trn1u_is_valid(&next))
		return V92_PCM_PHASE3_TRN1U_INVALID_STATE;
	*trn1u = next;
	*symbol = generated;
	return V92_PCM_PHASE3_TRN1U_OK;
}
