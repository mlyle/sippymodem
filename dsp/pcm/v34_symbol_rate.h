// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_SYMBOL_RATE_H
#define V34_SYMBOL_RATE_H

#include <stdbool.h>
#include <stddef.h>

enum v34_symbol_rate {
	V34_SYMBOL_RATE_2400,
	V34_SYMBOL_RATE_2743,
	V34_SYMBOL_RATE_2800,
	V34_SYMBOL_RATE_3000,
	V34_SYMBOL_RATE_3200,
	V34_SYMBOL_RATE_3429,
	V34_SYMBOL_RATE_COUNT,
};

/* Decode the integer baud labels used by configuration. Labels 2743 and
 * 3429 denote exact clocks 19200/7 and 24000/7, not rounded DSP clocks.
 * Zero (automatic selection) and protocol-specific rate restrictions belong
 * to the caller. Reject unknown labels or NULL without changing the output.
 * Match the AT parser width so a wide input cannot wrap into a valid label. */
static inline bool v34_symbol_rate_from_number(
	unsigned long number, enum v34_symbol_rate *symbol_rate)
{
	if (symbol_rate == NULL)
		return false;
	switch (number) {
	case 2400:
		*symbol_rate = V34_SYMBOL_RATE_2400;
		return true;
	case 2743:
		*symbol_rate = V34_SYMBOL_RATE_2743;
		return true;
	case 2800:
		*symbol_rate = V34_SYMBOL_RATE_2800;
		return true;
	case 3000:
		*symbol_rate = V34_SYMBOL_RATE_3000;
		return true;
	case 3200:
		*symbol_rate = V34_SYMBOL_RATE_3200;
		return true;
	case 3429:
		*symbol_rate = V34_SYMBOL_RATE_3429;
		return true;
	default:
		return false;
	}
}

#endif /* V34_SYMBOL_RATE_H */
