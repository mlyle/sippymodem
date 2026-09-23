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
	if (symbol == NULL || !g711_law_is_valid(law))
		return false;
	*symbol = (struct v90_pcm_symbol){
		.ucode = law == G711_LAW_PCMA ?
			(uint8_t)((octet ^ UINT8_C(0xd5)) & UINT8_C(0x7f)) :
			(uint8_t)(~octet & UINT8_C(0x7f)),
		.positive = (octet & UINT8_C(0x80)) != 0,
		.octet = octet,
		/* The linear path uses -1 to preserve PCMU's negative zero. */
		.sample = law == G711_LAW_PCMU && octet == UINT8_C(0x7f) ?
			-1 : g711_decode(law, octet),
	};
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
