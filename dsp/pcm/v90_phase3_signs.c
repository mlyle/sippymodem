// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_signs.h"

#include "v90_info1.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool v90_phase3_signs_is_valid(
	const struct v90_phase3_signs *signs)
{
	if (signs == NULL ||
	    !v34_scrambler_is_valid(&signs->scrambler) ||
	    signs->scrambler.polynomial != V34_POLYNOMIAL_GPC ||
	    !g711_law_is_valid(signs->law) || signs->last_sign > 1 ||
	    signs->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    signs->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    (signs->phase != V90_PHASE3_SIGNS_TRN1D &&
	     signs->phase != V90_PHASE3_SIGNS_JD))
		return false;
	if (signs->phase == V90_PHASE3_SIGNS_JD)
		return signs->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       signs->trn1d_symbols %
			       V90_PHASE3_TRN1D_ALIGNMENT == 0;
	if (signs->trn1d_symbols == 0)
		return signs->last_sign == 0 &&
		       signs->scrambler.history == 0;
	return true;
}

enum v90_phase3_sign_result v90_phase3_signs_init(
	struct v90_phase3_signs *signs, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase3_signs initialized = {
		.law = law,
		.phase = V90_PHASE3_SIGNS_TRN1D,
		.training_ucode = training_ucode,
	};

	if (signs == NULL || !g711_law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_PHASE3_SIGNS_INVALID_ARGUMENT;
	*signs = initialized;
	return V90_PHASE3_SIGNS_OK;
}

static bool make_symbol(
	const struct v90_phase3_signs *signs, uint8_t sign,
	struct v90_pcm_symbol *symbol)
{
	return v90_pcm_symbol_make(
		symbol, signs->law, signs->training_ucode, sign != 0);
}

enum v90_phase3_sign_result v90_phase3_signs_next_trn1d(
	struct v90_phase3_signs *signs,
	struct v90_pcm_symbol *symbol)
{
	struct v90_phase3_signs next;
	struct v90_pcm_symbol generated;
	uint8_t sign;

	if (signs == NULL || symbol == NULL)
		return V90_PHASE3_SIGNS_INVALID_ARGUMENT;
	if (!v90_phase3_signs_is_valid(signs) ||
	    signs->phase != V90_PHASE3_SIGNS_TRN1D ||
	    signs->trn1d_symbols == UINT32_MAX)
		return V90_PHASE3_SIGNS_INVALID_STATE;
	next = *signs;
	if (v34_scramble_group(
		    &next.scrambler, 1, 1, &sign) != V34_FRAMING_OK ||
	    !make_symbol(&next, sign, &generated))
		return V90_PHASE3_SIGNS_INVALID_STATE;
	next.last_sign = sign;
	next.trn1d_symbols++;
	*signs = next;
	*symbol = generated;
	return V90_PHASE3_SIGNS_OK;
}

enum v90_phase3_sign_result v90_phase3_signs_begin_jd(
	struct v90_phase3_signs *signs)
{
	if (signs == NULL)
		return V90_PHASE3_SIGNS_INVALID_ARGUMENT;
	if (!v90_phase3_signs_is_valid(signs) ||
	    signs->phase != V90_PHASE3_SIGNS_TRN1D ||
	    signs->trn1d_symbols < V90_PHASE3_TRN1D_MIN_SYMBOLS ||
	    signs->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT != 0)
		return V90_PHASE3_SIGNS_INVALID_STATE;
	signs->phase = V90_PHASE3_SIGNS_JD;
	return V90_PHASE3_SIGNS_OK;
}

enum v90_phase3_sign_result v90_phase3_signs_next_jd_bit(
	struct v90_phase3_signs *signs, uint8_t bit,
	struct v90_pcm_symbol *symbol)
{
	struct v90_phase3_signs next;
	struct v90_pcm_symbol generated;
	uint8_t scrambled;
	uint8_t sign;

	if (signs == NULL || symbol == NULL || bit > 1)
		return V90_PHASE3_SIGNS_INVALID_ARGUMENT;
	if (!v90_phase3_signs_is_valid(signs) ||
	    signs->phase != V90_PHASE3_SIGNS_JD)
		return V90_PHASE3_SIGNS_INVALID_STATE;
	next = *signs;
	if (v34_scramble_group(
		    &next.scrambler, bit, 1, &scrambled) !=
		    V34_FRAMING_OK)
		return V90_PHASE3_SIGNS_INVALID_STATE;
	sign = scrambled ^ next.last_sign;
	if (!make_symbol(&next, sign, &generated))
		return V90_PHASE3_SIGNS_INVALID_STATE;
	next.last_sign = sign;
	*signs = next;
	*symbol = generated;
	return V90_PHASE3_SIGNS_OK;
}

const char *v90_phase3_sign_result_name(
	enum v90_phase3_sign_result result)
{
	switch (result) {
	case V90_PHASE3_SIGNS_OK:
		return "ok";
	case V90_PHASE3_SIGNS_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_SIGNS_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
