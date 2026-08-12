// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool v90_pcm_symbol_make(
	struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, bool positive)
{
	struct v90_pcm_symbol generated;

	if (symbol == NULL || ucode > UINT8_C(127) ||
	    !g711_ucode_octet(law, ucode, positive, &generated.octet) ||
	    !g711_ucode_sample(law, ucode, positive, &generated.sample))
		return false;
	generated.ucode = ucode;
	generated.positive = positive;
	*symbol = generated;
	return true;
}
