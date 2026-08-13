// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_digital_prefix.h"

#include <stddef.h>

static bool stage_is_valid(enum pcm_phase3_digital_prefix_stage stage)
{
	return stage >= PCM_PHASE3_DIGITAL_PREFIX_SD &&
	       stage <= PCM_PHASE3_DIGITAL_PREFIX_COMPLETE;
}

bool pcm_phase3_digital_prefix_is_valid(
	const struct pcm_phase3_digital_prefix *prefix)
{
	if (prefix == NULL || !stage_is_valid(prefix->stage))
		return false;
	switch (prefix->stage) {
	case PCM_PHASE3_DIGITAL_PREFIX_SD:
		return v90_phase3_sd_is_valid(&prefix->sequence.sd) &&
		       prefix->sequence.sd.kind == V90_PHASE3_SD &&
		       v90_phase3_sd_remaining(&prefix->sequence.sd) != 0;
	case PCM_PHASE3_DIGITAL_PREFIX_SD_BAR:
		return v90_phase3_sd_is_valid(&prefix->sequence.sd) &&
		       prefix->sequence.sd.kind == V90_PHASE3_SD_BAR &&
		       v90_phase3_sd_remaining(&prefix->sequence.sd) != 0;
	case PCM_PHASE3_DIGITAL_PREFIX_TRN1D:
		return v90_phase3_signs_is_valid(&prefix->sequence.signs) &&
		       prefix->sequence.signs.phase == V90_PHASE3_SIGNS_TRN1D &&
		       prefix->sequence.signs.trn1d_symbols <
			       V90_PHASE3_TRN1D_MIN_SYMBOLS;
	case PCM_PHASE3_DIGITAL_PREFIX_COMPLETE:
		return v90_phase3_signs_is_valid(&prefix->sequence.signs) &&
		       prefix->sequence.signs.phase == V90_PHASE3_SIGNS_TRN1D &&
		       prefix->sequence.signs.trn1d_symbols ==
			       V90_PHASE3_TRN1D_MIN_SYMBOLS;
	}
	return false;
}

bool pcm_phase3_digital_prefix_is_complete(
	const struct pcm_phase3_digital_prefix *prefix)
{
	return pcm_phase3_digital_prefix_is_valid(prefix) &&
	       prefix->stage == PCM_PHASE3_DIGITAL_PREFIX_COMPLETE;
}

enum pcm_phase3_digital_prefix_result pcm_phase3_digital_prefix_init(
	struct pcm_phase3_digital_prefix *prefix, enum g711_law law,
	uint8_t training_ucode)
{
	struct pcm_phase3_digital_prefix initialized = {
		.stage = PCM_PHASE3_DIGITAL_PREFIX_SD,
	};

	if (prefix == NULL ||
	    v90_phase3_sd_init(
		    &initialized.sequence.sd, law, training_ucode,
		    V90_PHASE3_SD) != V90_PHASE3_SD_OK)
		return PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT;
	*prefix = initialized;
	return PCM_PHASE3_DIGITAL_PREFIX_OK;
}

static bool begin_sd_bar(struct pcm_phase3_digital_prefix *prefix)
{
	enum g711_law law = prefix->sequence.sd.law;
	uint8_t training_ucode = prefix->sequence.sd.training_ucode;

	prefix->sequence = (union pcm_phase3_digital_prefix_sequence){ 0 };
	if (v90_phase3_sd_init(
		    &prefix->sequence.sd, law, training_ucode,
		    V90_PHASE3_SD_BAR) != V90_PHASE3_SD_OK)
		return false;
	prefix->stage = PCM_PHASE3_DIGITAL_PREFIX_SD_BAR;
	return true;
}

static bool begin_trn1d(struct pcm_phase3_digital_prefix *prefix)
{
	enum g711_law law = prefix->sequence.sd.law;
	uint8_t training_ucode = prefix->sequence.sd.training_ucode;

	prefix->sequence = (union pcm_phase3_digital_prefix_sequence){ 0 };
	if (v90_phase3_signs_init(
		    &prefix->sequence.signs, law, training_ucode) !=
	    V90_PHASE3_SIGNS_OK)
		return false;
	prefix->stage = PCM_PHASE3_DIGITAL_PREFIX_TRN1D;
	return true;
}

static bool next_sd(
	struct pcm_phase3_digital_prefix *prefix,
	struct v90_pcm_symbol *symbol)
{
	enum v90_phase3_sd_kind kind = prefix->sequence.sd.kind;

	if (v90_phase3_sd_next(&prefix->sequence.sd, symbol) !=
	    V90_PHASE3_SD_OK)
		return false;
	if (v90_phase3_sd_remaining(&prefix->sequence.sd) != 0)
		return true;
	return kind == V90_PHASE3_SD ? begin_sd_bar(prefix) :
					     begin_trn1d(prefix);
}

static bool next_trn1d(
	struct pcm_phase3_digital_prefix *prefix,
	struct v90_pcm_symbol *symbol)
{
	if (v90_phase3_signs_next_trn1d(
		    &prefix->sequence.signs, symbol) != V90_PHASE3_SIGNS_OK)
		return false;
	if (prefix->sequence.signs.trn1d_symbols ==
	    V90_PHASE3_TRN1D_MIN_SYMBOLS)
		prefix->stage = PCM_PHASE3_DIGITAL_PREFIX_COMPLETE;
	return true;
}

enum pcm_phase3_digital_prefix_result pcm_phase3_digital_prefix_next(
	struct pcm_phase3_digital_prefix *prefix,
	struct v90_pcm_symbol *symbol)
{
	struct pcm_phase3_digital_prefix next;
	struct v90_pcm_symbol generated;
	bool advanced;

	if (!pcm_phase3_digital_prefix_is_valid(prefix) || symbol == NULL)
		return PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT;
	if (prefix->stage == PCM_PHASE3_DIGITAL_PREFIX_COMPLETE)
		return PCM_PHASE3_DIGITAL_PREFIX_DONE;
	next = *prefix;
	if (next.stage == PCM_PHASE3_DIGITAL_PREFIX_SD ||
	    next.stage == PCM_PHASE3_DIGITAL_PREFIX_SD_BAR)
		advanced = next_sd(&next, &generated);
	else
		advanced = next_trn1d(&next, &generated);
	if (!advanced || !pcm_phase3_digital_prefix_is_valid(&next))
		return PCM_PHASE3_DIGITAL_PREFIX_INVALID_STATE;
	*prefix = next;
	*symbol = generated;
	return PCM_PHASE3_DIGITAL_PREFIX_OK;
}

enum pcm_phase3_digital_prefix_result pcm_phase3_digital_prefix_handoff(
	const struct pcm_phase3_digital_prefix *prefix,
	struct v90_phase3_signs *signs)
{
	if (prefix == NULL || signs == NULL)
		return PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT;
	if (!pcm_phase3_digital_prefix_is_complete(prefix))
		return PCM_PHASE3_DIGITAL_PREFIX_INVALID_STATE;
	*signs = prefix->sequence.signs;
	return PCM_PHASE3_DIGITAL_PREFIX_OK;
}

const char *pcm_phase3_digital_prefix_result_name(
	enum pcm_phase3_digital_prefix_result result)
{
	switch (result) {
	case PCM_PHASE3_DIGITAL_PREFIX_OK:
		return "ok";
	case PCM_PHASE3_DIGITAL_PREFIX_DONE:
		return "done";
	case PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_PHASE3_DIGITAL_PREFIX_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
