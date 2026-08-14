// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_sd.h"

#include "v90_info1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool kind_is_valid(enum v90_phase3_sd_kind kind)
{
	return kind == V90_PHASE3_SD || kind == V90_PHASE3_SD_BAR;
}

bool v90_phase3_sd_is_valid(const struct v90_phase3_sd *sequence)
{
	uint16_t expected_total;

	if (sequence == NULL || !g711_law_is_valid(sequence->law) ||
	    !kind_is_valid(sequence->kind) ||
	    sequence->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    sequence->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE)
		return false;
	expected_total = sequence->kind == V90_PHASE3_SD ?
				 V90_PHASE3_SD_SYMBOLS :
				 V90_PHASE3_SD_BAR_SYMBOLS;
	return sequence->total_symbols == expected_total &&
	       sequence->emitted_symbols <= expected_total;
}

enum v90_phase3_sd_result v90_phase3_sd_init(
	struct v90_phase3_sd *sequence, enum g711_law law,
	uint8_t training_ucode, enum v90_phase3_sd_kind kind)
{
	struct v90_phase3_sd initialized;

	if (sequence == NULL || !g711_law_is_valid(law) ||
	    !kind_is_valid(kind) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE)
		return V90_PHASE3_SD_INVALID_ARGUMENT;
	initialized = (struct v90_phase3_sd){
		.law = law,
		.kind = kind,
		.training_ucode = training_ucode,
		.total_symbols = kind == V90_PHASE3_SD ?
					 V90_PHASE3_SD_SYMBOLS :
					 V90_PHASE3_SD_BAR_SYMBOLS,
	};
	*sequence = initialized;
	return V90_PHASE3_SD_OK;
}

static void symbol_values(
	const struct v90_phase3_sd *sequence,
	uint8_t *ucode, bool *positive)
{
	unsigned int phase = sequence->emitted_symbols %
			     V90_PHASE3_SD_PATTERN_SYMBOLS;
	bool inverted = sequence->kind == V90_PHASE3_SD_BAR;

	/* A Ucode is a seven-bit universal character; offset arithmetic stays
	 * in that code space. */
	*ucode = phase == 1 || phase == 4 ? 0 :
		 (uint8_t)((sequence->training_ucode +
			    V90_PHASE3_SD_TRAINING_OFFSET) & UINT8_C(0x7f));
	*positive = (phase < 3) != inverted;
}

enum v90_phase3_sd_result v90_phase3_sd_next(
	struct v90_phase3_sd *sequence, struct v90_pcm_symbol *symbol)
{
	struct v90_pcm_symbol generated;

	if (sequence == NULL || symbol == NULL)
		return V90_PHASE3_SD_INVALID_ARGUMENT;
	if (!v90_phase3_sd_is_valid(sequence))
		return V90_PHASE3_SD_INVALID_STATE;
	if (sequence->emitted_symbols == sequence->total_symbols)
		return V90_PHASE3_SD_COMPLETE;
	symbol_values(sequence, &generated.ucode, &generated.positive);
	if (!v90_pcm_symbol_make(
		    &generated, sequence->law, generated.ucode,
		    generated.positive))
		return V90_PHASE3_SD_INVALID_STATE;
	*symbol = generated;
	sequence->emitted_symbols++;
	return V90_PHASE3_SD_OK;
}

size_t v90_phase3_sd_remaining(
	const struct v90_phase3_sd *sequence)
{
	return v90_phase3_sd_is_valid(sequence) ?
		       (size_t)(sequence->total_symbols -
				sequence->emitted_symbols) :
		       0;
}

const char *v90_phase3_sd_result_name(
	enum v90_phase3_sd_result result)
{
	switch (result) {
	case V90_PHASE3_SD_OK:
		return "ok";
	case V90_PHASE3_SD_COMPLETE:
		return "complete";
	case V90_PHASE3_SD_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_SD_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
