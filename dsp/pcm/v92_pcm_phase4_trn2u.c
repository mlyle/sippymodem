// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase4_trn2u.h"

#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>

bool v92_pcm_phase4_trn2u_is_valid(
	const struct v92_pcm_phase4_trn2u *trn2u)
{
	return trn2u != NULL &&
	       v92_phase4_modulator_is_valid(&trn2u->modulator);
}

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_init(
	struct v92_pcm_phase4_trn2u *trn2u,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu)
{
	struct v92_pcm_phase4_trn2u initialized = { 0 };

	if (trn2u == NULL ||
	    v92_phase4_modulator_init(
		    &initialized.modulator, constellation,
		    previous_encoded_sign, lu) != V92_PHASE4_MODULATOR_OK ||
	    !v92_pcm_phase4_trn2u_is_valid(&initialized))
		return V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT;
	*trn2u = initialized;
	return V92_PCM_PHASE4_TRN2U_OK;
}

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_next(
	struct v92_pcm_phase4_trn2u *trn2u, int16_t *sample)
{
	unsigned int bits;
	int16_t generated;

	if (trn2u == NULL || sample == NULL ||
	    memory_regions_overlap(trn2u, sizeof(*trn2u), sample, sizeof(*sample)))
		return V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT;
	if (!v92_pcm_phase4_trn2u_is_valid(trn2u) ||
	    trn2u->emitted_symbols == UINT32_MAX)
		return V92_PCM_PHASE4_TRN2U_INVALID_STATE;
	bits = v92_phase4_modulator_bits_per_symbol(
		trn2u->modulator.constellation);
	if (v92_phase4_modulator_next(
		    &trn2u->modulator, (uint8_t)((1u << bits) - 1u),
		    &generated) != V92_PHASE4_MODULATOR_OK)
		return V92_PCM_PHASE4_TRN2U_INVALID_STATE;
	trn2u->emitted_symbols++;
	*sample = generated;
	return V92_PCM_PHASE4_TRN2U_OK;
}

const char *v92_pcm_phase4_trn2u_result_name(
	enum v92_pcm_phase4_trn2u_result result)
{
	switch (result) {
	case V92_PCM_PHASE4_TRN2U_OK:
		return "ok";
	case V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE4_TRN2U_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
