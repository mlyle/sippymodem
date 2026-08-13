// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_opening.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static size_t md_symbols(const struct v92_pcm_phase3_opening *opening)
{
	return (size_t)opening->md_length_276_symbol_units *
	       V92_PCM_PHASE3_MD_UNIT_SYMBOLS;
}

static bool ru_state_is_valid(
	const struct v92_pcm_phase3_opening *opening,
	enum v92_pcm_phase3_ru_kind kind)
{
	const struct v92_pcm_phase3_ru *ru = &opening->state.ru;

	return v92_pcm_phase3_ru_is_valid(ru) && ru->law == opening->law &&
	       ru->lu_ucode == opening->lu_ucode &&
	       ru->kind == kind && v92_pcm_phase3_ru_remaining(ru) != 0u;
}

bool v92_pcm_phase3_opening_is_valid(
	const struct v92_pcm_phase3_opening *opening)
{
	if (opening == NULL || !law_is_valid(opening->law) ||
	    opening->lu_ucode == 0u || opening->lu_ucode > 127u ||
	    opening->md_length_276_symbol_units >
		    V92_PCM_INFO1A_MAX_MD_LENGTH_UNITS)
		return false;
	switch (opening->stage) {
	case V92_PCM_PHASE3_OPENING_SILENCE:
		return opening->state.emitted_symbols <
		       V92_PCM_PHASE3_OPENING_SILENCE_SYMBOLS;
	case V92_PCM_PHASE3_OPENING_RU:
		return ru_state_is_valid(opening, V92_PCM_PHASE3_RU);
	case V92_PCM_PHASE3_OPENING_RU_BAR:
		return ru_state_is_valid(opening, V92_PCM_PHASE3_RU_BAR);
	case V92_PCM_PHASE3_OPENING_MD:
		return opening->md_length_276_symbol_units != 0u &&
		       opening->state.emitted_symbols < md_symbols(opening);
	case V92_PCM_PHASE3_OPENING_SECOND_RU:
		return opening->md_length_276_symbol_units != 0u &&
		       ru_state_is_valid(opening, V92_PCM_PHASE3_RU);
	case V92_PCM_PHASE3_OPENING_SECOND_RU_BAR:
		return opening->md_length_276_symbol_units != 0u &&
		       ru_state_is_valid(opening, V92_PCM_PHASE3_RU_BAR);
	case V92_PCM_PHASE3_OPENING_TRN1U_READY:
		return opening->state.emitted_symbols == 0u;
	}
	return false;
}

bool v92_pcm_phase3_opening_is_complete(
	const struct v92_pcm_phase3_opening *opening)
{
	return v92_pcm_phase3_opening_is_valid(opening) &&
	       opening->stage == V92_PCM_PHASE3_OPENING_TRN1U_READY;
}

enum v92_pcm_phase3_opening_result v92_pcm_phase3_opening_init(
	struct v92_pcm_phase3_opening *opening, enum g711_law law,
	uint8_t lu_ucode, const struct v92_pcm_info1a *info1a)
{
	struct v92_pcm_phase3_opening initialized = {
		.stage = V92_PCM_PHASE3_OPENING_SILENCE,
		.law = law,
	};

	if (opening == NULL || !law_is_valid(law) || lu_ucode == 0u ||
	    lu_ucode > 127u ||
	    !v92_pcm_info1a_is_valid(info1a))
		return V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT;
	initialized.lu_ucode = lu_ucode;
	initialized.md_length_276_symbol_units =
		info1a->local_md_length_276_symbol_units;
	*opening = initialized;
	return V92_PCM_PHASE3_OPENING_OK;
}

static bool start_ru(
	struct v92_pcm_phase3_opening *opening,
	enum v92_pcm_phase3_opening_stage stage,
	enum v92_pcm_phase3_ru_kind kind)
{
	struct v92_pcm_phase3_ru ru;

	if (v92_pcm_phase3_ru_init(
		    &ru, opening->law, opening->lu_ucode, kind) !=
	    V92_PCM_PHASE3_RU_OK)
		return false;
	opening->state = (union v92_pcm_phase3_opening_state){ .ru = ru };
	opening->stage = stage;
	return true;
}

static void start_scalar_stage(
	struct v92_pcm_phase3_opening *opening,
	enum v92_pcm_phase3_opening_stage stage)
{
	opening->state = (union v92_pcm_phase3_opening_state){ 0 };
	opening->stage = stage;
}

static bool symbol_is_valid(
	enum g711_law law, const struct v90_pcm_symbol *symbol)
{
	struct v90_pcm_symbol parsed;

	return symbol != NULL &&
	       v90_pcm_symbol_parse(&parsed, law, symbol->octet) &&
	       parsed.ucode == symbol->ucode &&
	       parsed.positive == symbol->positive &&
	       parsed.sample == symbol->sample;
}

static bool advance_after_ru(struct v92_pcm_phase3_opening *opening)
{
	switch (opening->stage) {
	case V92_PCM_PHASE3_OPENING_RU:
		return start_ru(
			opening, V92_PCM_PHASE3_OPENING_RU_BAR,
			V92_PCM_PHASE3_RU_BAR);
	case V92_PCM_PHASE3_OPENING_RU_BAR:
		start_scalar_stage(
			opening,
			opening->md_length_276_symbol_units == 0u ?
				V92_PCM_PHASE3_OPENING_TRN1U_READY :
				V92_PCM_PHASE3_OPENING_MD);
		return true;
	case V92_PCM_PHASE3_OPENING_SECOND_RU:
		return start_ru(
			opening, V92_PCM_PHASE3_OPENING_SECOND_RU_BAR,
			V92_PCM_PHASE3_RU_BAR);
	case V92_PCM_PHASE3_OPENING_SECOND_RU_BAR:
		start_scalar_stage(opening, V92_PCM_PHASE3_OPENING_TRN1U_READY);
		return true;
	default:
		return false;
	}
}

enum v92_pcm_phase3_opening_result v92_pcm_phase3_opening_next(
	struct v92_pcm_phase3_opening *opening,
	const struct v90_pcm_symbol *md_symbol,
	struct v90_pcm_symbol *symbol)
{
	struct v92_pcm_phase3_opening next;
	struct v90_pcm_symbol generated;

	if (opening == NULL || symbol == NULL)
		return V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_opening_is_valid(opening))
		return V92_PCM_PHASE3_OPENING_INVALID_STATE;
	if (opening->stage != V92_PCM_PHASE3_OPENING_MD &&
	    md_symbol != NULL)
		return V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT;
	if (opening->stage == V92_PCM_PHASE3_OPENING_TRN1U_READY)
		return V92_PCM_PHASE3_OPENING_COMPLETE;
	if (opening->stage == V92_PCM_PHASE3_OPENING_MD && md_symbol == NULL)
		return V92_PCM_PHASE3_OPENING_NEED_MD_SYMBOL;
	if (opening->stage == V92_PCM_PHASE3_OPENING_MD &&
	    !symbol_is_valid(opening->law, md_symbol))
		return V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT;

	next = *opening;
	switch (next.stage) {
	case V92_PCM_PHASE3_OPENING_SILENCE:
		if (!v90_pcm_symbol_make(&generated, next.law, 0u, true))
			return V92_PCM_PHASE3_OPENING_INVALID_STATE;
		next.state.emitted_symbols++;
		if (next.state.emitted_symbols ==
		    V92_PCM_PHASE3_OPENING_SILENCE_SYMBOLS &&
		    !start_ru(
			    &next, V92_PCM_PHASE3_OPENING_RU,
			    V92_PCM_PHASE3_RU))
			return V92_PCM_PHASE3_OPENING_INVALID_STATE;
		break;
	case V92_PCM_PHASE3_OPENING_RU:
	case V92_PCM_PHASE3_OPENING_RU_BAR:
	case V92_PCM_PHASE3_OPENING_SECOND_RU:
	case V92_PCM_PHASE3_OPENING_SECOND_RU_BAR:
		if (v92_pcm_phase3_ru_next(&next.state.ru, &generated) !=
		    V92_PCM_PHASE3_RU_OK)
			return V92_PCM_PHASE3_OPENING_INVALID_STATE;
		if (v92_pcm_phase3_ru_remaining(&next.state.ru) == 0u &&
		    !advance_after_ru(&next))
			return V92_PCM_PHASE3_OPENING_INVALID_STATE;
		break;
	case V92_PCM_PHASE3_OPENING_MD:
		generated = *md_symbol;
		next.state.emitted_symbols++;
		if (next.state.emitted_symbols == md_symbols(&next) &&
		    !start_ru(
			    &next, V92_PCM_PHASE3_OPENING_SECOND_RU,
			    V92_PCM_PHASE3_RU))
			return V92_PCM_PHASE3_OPENING_INVALID_STATE;
		break;
	case V92_PCM_PHASE3_OPENING_TRN1U_READY:
		return V92_PCM_PHASE3_OPENING_COMPLETE;
	}
	if (!v92_pcm_phase3_opening_is_valid(&next))
		return V92_PCM_PHASE3_OPENING_INVALID_STATE;
	*opening = next;
	*symbol = generated;
	return V92_PCM_PHASE3_OPENING_OK;
}

size_t v92_pcm_phase3_opening_remaining(
	const struct v92_pcm_phase3_opening *opening)
{
	size_t optional;

	if (!v92_pcm_phase3_opening_is_valid(opening))
		return 0u;
	optional = 0u;
	if (opening->md_length_276_symbol_units != 0u)
		optional = md_symbols(opening) + V92_PCM_PHASE3_RU_SYMBOLS +
			   V92_PCM_PHASE3_RU_BAR_SYMBOLS;
	switch (opening->stage) {
	case V92_PCM_PHASE3_OPENING_SILENCE:
		return V92_PCM_PHASE3_OPENING_SILENCE_SYMBOLS -
			       opening->state.emitted_symbols +
		       V92_PCM_PHASE3_RU_SYMBOLS +
		       V92_PCM_PHASE3_RU_BAR_SYMBOLS + optional;
	case V92_PCM_PHASE3_OPENING_RU:
		return v92_pcm_phase3_ru_remaining(&opening->state.ru) +
		       V92_PCM_PHASE3_RU_BAR_SYMBOLS + optional;
	case V92_PCM_PHASE3_OPENING_RU_BAR:
		return v92_pcm_phase3_ru_remaining(&opening->state.ru) +
		       optional;
	case V92_PCM_PHASE3_OPENING_MD:
		return md_symbols(opening) - opening->state.emitted_symbols +
		       V92_PCM_PHASE3_RU_SYMBOLS +
		       V92_PCM_PHASE3_RU_BAR_SYMBOLS;
	case V92_PCM_PHASE3_OPENING_SECOND_RU:
		return v92_pcm_phase3_ru_remaining(&opening->state.ru) +
		       V92_PCM_PHASE3_RU_BAR_SYMBOLS;
	case V92_PCM_PHASE3_OPENING_SECOND_RU_BAR:
		return v92_pcm_phase3_ru_remaining(&opening->state.ru);
	case V92_PCM_PHASE3_OPENING_TRN1U_READY:
		return 0u;
	}
	return 0u;
}

const char *v92_pcm_phase3_opening_stage_name(
	enum v92_pcm_phase3_opening_stage stage)
{
	switch (stage) {
	case V92_PCM_PHASE3_OPENING_SILENCE:
		return "silence";
	case V92_PCM_PHASE3_OPENING_RU:
		return "Ru";
	case V92_PCM_PHASE3_OPENING_RU_BAR:
		return "Ru-bar";
	case V92_PCM_PHASE3_OPENING_MD:
		return "MD";
	case V92_PCM_PHASE3_OPENING_SECOND_RU:
		return "second Ru";
	case V92_PCM_PHASE3_OPENING_SECOND_RU_BAR:
		return "second Ru-bar";
	case V92_PCM_PHASE3_OPENING_TRN1U_READY:
		return "TRN1u ready";
	}
	return "unknown";
}

const char *v92_pcm_phase3_opening_result_name(
	enum v92_pcm_phase3_opening_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_OPENING_OK:
		return "ok";
	case V92_PCM_PHASE3_OPENING_NEED_MD_SYMBOL:
		return "need MD symbol";
	case V92_PCM_PHASE3_OPENING_COMPLETE:
		return "complete";
	case V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_OPENING_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
