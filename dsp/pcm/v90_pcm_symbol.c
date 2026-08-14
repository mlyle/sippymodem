// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool v90_pcm_symbol_make(
	struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, bool positive)
{
	struct v90_pcm_symbol generated = { 0 };

	if (symbol == NULL || ucode > UINT8_C(127) ||
	    !g711_ucode_octet(law, ucode, positive, &generated.octet) ||
	    !g711_ucode_sample(law, ucode, positive, &generated.sample))
		return false;
	generated.ucode = ucode;
	generated.positive = positive;
	*symbol = generated;
	return true;
}

bool v90_pcm_symbol_parse(
	struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t octet)
{
	struct v90_pcm_symbol parsed = { 0 };
	uint8_t ucode = law == G711_LAW_PCMA ?
			(uint8_t)((octet ^ UINT8_C(0xd5)) & UINT8_C(0x7f)) :
			(uint8_t)(~octet & UINT8_C(0x7f));
	bool positive = (octet & UINT8_C(0x80)) != 0;

	if (symbol == NULL ||
	    !v90_pcm_symbol_make(&parsed, law, ucode, positive) ||
	    parsed.octet != octet)
		return false;
	*symbol = parsed;
	return true;
}

bool v90_pcm_symbol_is_valid(
	const struct v90_pcm_symbol *symbol, enum g711_law law)
{
	struct v90_pcm_symbol parsed;

	return symbol != NULL &&
	       v90_pcm_symbol_parse(&parsed, law, symbol->octet) &&
	       parsed.ucode == symbol->ucode &&
	       parsed.positive == symbol->positive &&
	       parsed.sample == symbol->sample;
}
