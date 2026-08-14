// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase4_trn2u.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

unsigned int v92_pcm_phase4_trn2u_bits_per_symbol(
	enum v92_constellation_size constellation)
{
	switch (constellation) {
	case V92_CONSTELLATION_4_POINT:
		return 2;
	case V92_CONSTELLATION_8_POINT:
		return 3;
	}
	return 0;
}

static unsigned int magnitude_count(
	enum v92_constellation_size constellation)
{
	unsigned int bits =
		v92_pcm_phase4_trn2u_bits_per_symbol(constellation);

	return bits == 0 ? 0 : 1u << (bits - 1u);
}

static bool raw_level(
	enum v92_constellation_size constellation, int16_t lu,
	uint8_t magnitude_index, uint8_t encoded_sign, int16_t *sample)
{
	double mean_square;
	double scaled;
	long rounded;

	if (sample == NULL || lu <= 0 || encoded_sign > 1u ||
	    magnitude_index >= magnitude_count(constellation))
		return false;
	mean_square = ((double)constellation * (double)constellation - 1.0) /
		3.0;
	scaled = (double)(2u * magnitude_index + 1u) * (double)lu /
		sqrt(mean_square);
	rounded = lround(scaled);
	if (rounded <= 0 || rounded > INT16_MAX)
		return false;
	*sample = encoded_sign != 0u ?
		(int16_t)-rounded : (int16_t)rounded;
	return true;
}

static bool parameters_are_valid(
	enum v92_constellation_size constellation, int16_t lu)
{
	unsigned int count = magnitude_count(constellation);
	int16_t outer_level;

	return count != 0 && raw_level(
		constellation, lu, (uint8_t)(count - 1u), 0u,
		&outer_level);
}

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_level(
	enum v92_constellation_size constellation, int16_t lu,
	uint8_t magnitude_index, uint8_t encoded_sign, int16_t *sample)
{
	int16_t generated;

	if (sample == NULL || !parameters_are_valid(constellation, lu) ||
	    !raw_level(
		    constellation, lu, magnitude_index, encoded_sign,
		    &generated))
		return V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT;
	*sample = generated;
	return V92_PCM_PHASE4_TRN2U_OK;
}

bool v92_pcm_phase4_trn2u_is_valid(
	const struct v92_pcm_phase4_trn2u *trn2u)
{
	return trn2u != NULL &&
	       parameters_are_valid(trn2u->constellation, trn2u->lu) &&
	       trn2u->previous_encoded_sign <= 1u &&
	       v34_scrambler_is_valid(&trn2u->scrambler) &&
	       trn2u->scrambler.polynomial == V34_POLYNOMIAL_GPA;
}

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_init(
	struct v92_pcm_phase4_trn2u *trn2u,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu)
{
	struct v92_pcm_phase4_trn2u initialized = {
		.constellation = constellation,
		.lu = lu,
		.previous_encoded_sign = previous_encoded_sign,
	};

	if (trn2u == NULL || previous_encoded_sign > 1u ||
	    !parameters_are_valid(constellation, lu) ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK ||
	    !v92_pcm_phase4_trn2u_is_valid(&initialized))
		return V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT;
	*trn2u = initialized;
	return V92_PCM_PHASE4_TRN2U_OK;
}

bool v92_pcm_phase4_trn2u_can_handoff(
	const struct v92_pcm_phase4_trn2u *trn2u)
{
	return v92_pcm_phase4_trn2u_is_valid(trn2u) &&
	       trn2u->emitted_symbols >=
		       V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
	       trn2u->emitted_symbols % V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u;
}

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_next(
	struct v92_pcm_phase4_trn2u *trn2u, int16_t *sample)
{
	struct v92_pcm_phase4_trn2u next;
	unsigned int bits;
	uint8_t group = 0;
	uint8_t encoded_sign;
	int16_t generated;

	if (trn2u == NULL || sample == NULL)
		return V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT;
	if (!v92_pcm_phase4_trn2u_is_valid(trn2u) ||
	    trn2u->emitted_symbols == UINT32_MAX)
		return V92_PCM_PHASE4_TRN2U_INVALID_STATE;
	next = *trn2u;
	bits = v92_pcm_phase4_trn2u_bits_per_symbol(next.constellation);
	for (unsigned int bit = 0; bit < bits; ++bit) {
		uint8_t scrambled;

		if (v34_scramble_group(
			    &next.scrambler, 1u, 1u, &scrambled) !=
		    V34_FRAMING_OK)
			return V92_PCM_PHASE4_TRN2U_INVALID_STATE;
		group |= (uint8_t)(scrambled << bit);
	}
	encoded_sign = (uint8_t)(
		(group >> (bits - 1u)) ^ next.previous_encoded_sign);
	if (!raw_level(
		    next.constellation, next.lu,
		    (uint8_t)(group & ((1u << (bits - 1u)) - 1u)),
		    encoded_sign, &generated))
		return V92_PCM_PHASE4_TRN2U_INVALID_STATE;
	next.previous_encoded_sign = encoded_sign;
	next.emitted_symbols++;
	if (!v92_pcm_phase4_trn2u_is_valid(&next))
		return V92_PCM_PHASE4_TRN2U_INVALID_STATE;
	*trn2u = next;
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
