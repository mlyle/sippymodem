// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_ru.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static bool kind_is_valid(enum v92_pcm_phase3_ru_kind kind)
{
	return kind == V92_PCM_PHASE3_RU ||
	       kind == V92_PCM_PHASE3_RU_BAR;
}

static uint16_t total_symbols(enum v92_pcm_phase3_ru_kind kind)
{
	return kind == V92_PCM_PHASE3_RU ?
		       V92_PCM_PHASE3_RU_SYMBOLS :
		       V92_PCM_PHASE3_RU_BAR_SYMBOLS;
}

bool v92_pcm_phase3_ru_is_valid(
	const struct v92_pcm_phase3_ru *sequence)
{
	return sequence != NULL && law_is_valid(sequence->law) &&
	       kind_is_valid(sequence->kind) &&
	       sequence->lu_ucode > 0u && sequence->lu_ucode <= 127u &&
	       sequence->emitted_symbols <= total_symbols(sequence->kind);
}

enum v92_pcm_phase3_ru_result v92_pcm_phase3_ru_init(
	struct v92_pcm_phase3_ru *sequence, enum g711_law law,
	uint8_t lu_ucode, enum v92_pcm_phase3_ru_kind kind)
{
	struct v92_pcm_phase3_ru initialized = {
		.law = law,
		.kind = kind,
		.lu_ucode = lu_ucode,
	};

	if (sequence == NULL || !law_is_valid(law) || !kind_is_valid(kind) ||
	    lu_ucode == 0u || lu_ucode > 127u)
		return V92_PCM_PHASE3_RU_INVALID_ARGUMENT;
	*sequence = initialized;
	return V92_PCM_PHASE3_RU_OK;
}

enum v92_pcm_phase3_ru_result v92_pcm_phase3_ru_next(
	struct v92_pcm_phase3_ru *sequence,
	struct v90_pcm_symbol *symbol)
{
	struct v90_pcm_symbol generated;
	unsigned int phase;
	bool positive;

	if (sequence == NULL || symbol == NULL)
		return V92_PCM_PHASE3_RU_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_ru_is_valid(sequence))
		return V92_PCM_PHASE3_RU_INVALID_STATE;
	if (sequence->emitted_symbols == total_symbols(sequence->kind))
		return V92_PCM_PHASE3_RU_COMPLETE;
	phase = sequence->emitted_symbols % V92_PCM_PHASE3_RU_PATTERN_SYMBOLS;
	positive = (phase < 3u) !=
		   (sequence->kind == V92_PCM_PHASE3_RU_BAR);
	if (!v90_pcm_symbol_make(
		    &generated, sequence->law, sequence->lu_ucode,
		    positive))
		return V92_PCM_PHASE3_RU_INVALID_STATE;
	sequence->emitted_symbols++;
	*symbol = generated;
	return V92_PCM_PHASE3_RU_OK;
}

size_t v92_pcm_phase3_ru_remaining(
	const struct v92_pcm_phase3_ru *sequence)
{
	return v92_pcm_phase3_ru_is_valid(sequence) ?
		       (size_t)(total_symbols(sequence->kind) -
			sequence->emitted_symbols) :
		       0u;
}

const char *v92_pcm_phase3_ru_result_name(
	enum v92_pcm_phase3_ru_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_RU_OK:
		return "ok";
	case V92_PCM_PHASE3_RU_COMPLETE:
		return "complete";
	case V92_PCM_PHASE3_RU_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_RU_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
