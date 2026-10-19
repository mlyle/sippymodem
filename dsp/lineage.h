// SPDX-License-Identifier: BSD-3-Clause

#ifndef LINEAGE_H
#define LINEAGE_H

#include <stdint.h>

#define LINEAGE_INITIAL UINT64_C(14695981039346656037)

/* Deterministic history tags for delayed DSP observations. Mix whole 16-bit
 * words, with unsigned wrap at 64 bits; this is not a byte-stream hash or a
 * secret identity. Owners choose their event tags and field order. Identical
 * copied owners intentionally retain compatible observations. */
static inline uint64_t lineage_word(uint64_t lineage, uint16_t word)
{
	return (lineage ^ word) * UINT64_C(1099511628211);
}

/* Separate a control/event tag from the ordinary sample and decision words. */
static inline uint64_t lineage_tag(uint64_t lineage, uint16_t tag)
{
	return lineage_word(lineage_word(lineage, UINT16_C(0xa55a)), tag);
}

#endif /* LINEAGE_H */
