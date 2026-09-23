// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PCM_SYMBOL_H
#define V90_PCM_SYMBOL_H

#include "g711.h"

#include <stdbool.h>
#include <stdint.h>

/* octet is authoritative at a digital network interface.  sample is the
 * exact-round-trip representation used by the signed-linear modem path. */
struct v90_pcm_symbol {
	uint8_t ucode;
	bool positive;
	uint8_t octet;
	int16_t sample;
};

bool v90_pcm_symbol_make(
	struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, bool positive);

/* Recover the universal code and sign directly from the network character,
 * then decode its linear sample. This preserves PCMU negative zero. */
bool v90_pcm_symbol_parse(
	struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t octet);

/* Verify that all redundant representations describe one exact network
 * character. */
bool v90_pcm_symbol_is_valid(
	const struct v90_pcm_symbol *symbol, enum g711_law law);

#endif
