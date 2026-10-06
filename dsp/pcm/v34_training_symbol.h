// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_SYMBOL_H
#define V34_TRAINING_SYMBOL_H

#include "v34_constellation.h"
#include "v34_framing.h"

/* V.34 10.1.3.9 and V.90 8.5.2: scramble the differential dibit first,
 * then the quarter-point dibit for 16-point symbols, and rotate clockwise.
 * The owner supplies private scrambler and phase candidates plus bounded
 * dibits; it discards both candidates on failure. TRN uses absolute
 * rotation instead. */
static inline bool v34_training_encode_symbol(
	struct v34_scrambler *candidate,
	enum v34_constellation_size constellation,
	uint8_t differential_dibit, uint8_t mapped_dibit,
	uint8_t *rotation, struct qam_point *point, uint8_t *quarter_point)
{
	uint8_t quarter = 0, turns;

	if (v34_scramble_differential_dibit(candidate, differential_dibit,
		    *rotation, &turns) != V34_FRAMING_OK ||
	    (constellation == V34_CONSTELLATION_16_POINT &&
	     v34_scramble_mapped_dibit(candidate, mapped_dibit, &quarter) !=
		     V34_FRAMING_OK) ||
	    v34_training_constellation_point(constellation, quarter, turns, point) !=
		    V34_CONSTELLATION_OK)
		return false;
	*quarter_point = quarter;
	*rotation = turns;
	return true;
}

#endif /* V34_TRAINING_SYMBOL_H */
