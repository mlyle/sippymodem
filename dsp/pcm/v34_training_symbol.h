// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_SYMBOL_H
#define V34_TRAINING_SYMBOL_H

#include "packed_bits.h"
#include "v34_constellation.h"
#include "v34_framing.h"

/* V.34 10.1.3.9 and V.90 8.5.2: scramble the differential dibit first,
 * then the quarter-point dibit for 16-point symbols, and rotate clockwise.
 * The owner validates non-NULL, disjoint state/output regions. History,
 * phase and outputs commit together after encoding succeeds. Rejection
 * preserves them all. TRN uses absolute rotation instead. */
static inline bool v34_training_encode_symbol(
	struct v34_scrambler *scrambler,
	enum v34_constellation_size constellation,
	uint8_t differential_dibit, uint8_t mapped_dibit,
	uint8_t *rotation, struct qam_point *point, uint8_t *quarter_point)
{
	struct v34_scrambler next = *scrambler;
	struct qam_point generated;
	uint8_t quarter = 0, turns;

	if (v34_scramble_differential_dibit(&next, differential_dibit,
		    *rotation, &turns) != V34_FRAMING_OK ||
	    (constellation == V34_CONSTELLATION_16_POINT &&
	     v34_scramble_mapped_dibit(&next, mapped_dibit, &quarter) !=
		     V34_FRAMING_OK) ||
	    v34_training_constellation_point(constellation, quarter, turns, &generated) !=
		    V34_CONSTELLATION_OK)
		return false;
	*scrambler = next;
	*point = generated;
	*quarter_point = quarter;
	*rotation = turns;
	return true;
}

/* The record owner validates a complete two- or four-bit symbol at bit_index.
 * Read its differential dibit first, followed by the mapped dibit when used.
 * The record stays read-only; the same atomic history/output update applies. */
static inline bool v34_training_encode_packed_symbol(
	struct v34_scrambler *scrambler,
	enum v34_constellation_size constellation,
	const uint8_t *frame, size_t bit_index,
	uint8_t *rotation, struct qam_point *point, uint8_t *quarter_point)
{
	return v34_training_encode_symbol(scrambler, constellation,
		(uint8_t)packed_msb_read_msb_field(frame, bit_index, 2u),
		constellation == V34_CONSTELLATION_16_POINT ?
			(uint8_t)packed_msb_read_msb_field(frame, bit_index + 2u, 2u) : 0u,
		rotation, point, quarter_point);
}

#endif /* V34_TRAINING_SYMBOL_H */
