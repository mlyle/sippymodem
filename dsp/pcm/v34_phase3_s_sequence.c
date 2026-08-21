// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_s_sequence.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE3_S_SEQUENCE_SYMBOLS <= UINT16_MAX,
	"V.34 S/S-bar sequence counter must hold the complete sequence");

bool v34_phase3_s_sequence_is_valid(
	const struct v34_phase3_s_sequence *sequence)
{
	return sequence != NULL &&
	       sequence->emitted_symbols <=
		       V34_PHASE3_S_SEQUENCE_SYMBOLS;
}

bool v34_phase3_s_sequence_is_complete(
	const struct v34_phase3_s_sequence *sequence)
{
	return v34_phase3_s_sequence_is_valid(sequence) &&
	       sequence->emitted_symbols ==
		       V34_PHASE3_S_SEQUENCE_SYMBOLS;
}

enum v34_phase3_s_sequence_result
v34_phase3_s_sequence_init(
	struct v34_phase3_s_sequence *sequence)
{
	if (sequence == NULL)
		return V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT;
	*sequence = (struct v34_phase3_s_sequence){ 0 };
	return V34_PHASE3_S_SEQUENCE_OK;
}

enum v34_phase3_s_sequence_result
v34_phase3_s_sequence_next(
	struct v34_phase3_s_sequence *sequence,
	struct qam_point *point,
	enum v34_phase3_s_section *section)
{
	struct v34_phase3_s_sequence next;
	struct qam_point next_point;
	enum v34_phase3_s_section next_section;
	unsigned int quarter_turns;
	uint16_t symbol;

	if (!v34_phase3_s_sequence_is_valid(sequence) ||
	    point == NULL || section == NULL ||
	    memory_regions_overlap(
		    sequence, sequence == NULL ? 0 : sizeof(*sequence),
		    point, sizeof(*point)) ||
	    memory_regions_overlap(
		    sequence, sequence == NULL ? 0 : sizeof(*sequence),
		    section, sizeof(*section)) ||
	    memory_regions_overlap(
		    point, sizeof(*point), section, sizeof(*section)))
		return V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT;
	if (v34_phase3_s_sequence_is_complete(sequence))
		return V34_PHASE3_S_SEQUENCE_COMPLETE;
	next = *sequence;
	symbol = next.emitted_symbols;
	if (symbol < V34_PHASE3_S_SYMBOLS) {
		next_section = V34_PHASE3_S_SECTION;
		quarter_turns = symbol & 1u;
	} else {
		next_section = V34_PHASE3_S_BAR_SECTION;
		quarter_turns = 2u +
			((symbol - V34_PHASE3_S_SYMBOLS) & 1u);
	}
	next_point = v34_rotated_point_zero((int)quarter_turns);
	next.emitted_symbols++;
	*sequence = next;
	*point = next_point;
	*section = next_section;
	return V34_PHASE3_S_SEQUENCE_OK;
}

enum v34_phase3_s_sequence_result
v34_phase3_s_sequence_next_fixed(
	struct v34_phase3_s_sequence *sequence,
	struct v34_analytic_sample *point,
	enum v34_phase3_s_section *section)
{
	struct v34_phase3_s_sequence next;
	struct v34_analytic_sample next_point;
	enum v34_phase3_s_section next_section;
	unsigned int counterclockwise_turns;
	uint16_t symbol;

	if (!v34_phase3_s_sequence_is_valid(sequence) ||
	    point == NULL || section == NULL ||
	    memory_regions_overlap(
		    sequence, sequence == NULL ? 0 : sizeof(*sequence),
		    point, sizeof(*point)) ||
	    memory_regions_overlap(
		    sequence, sequence == NULL ? 0 : sizeof(*sequence),
		    section, sizeof(*section)) ||
	    memory_regions_overlap(
		    point, sizeof(*point), section, sizeof(*section)))
		return V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT;
	if (v34_phase3_s_sequence_is_complete(sequence))
		return V34_PHASE3_S_SEQUENCE_COMPLETE;
	next = *sequence;
	symbol = next.emitted_symbols;
	if (symbol < V34_PHASE3_S_SYMBOLS) {
		next_section = V34_PHASE3_S_SECTION;
		counterclockwise_turns = symbol & 1u;
	} else {
		next_section = V34_PHASE3_S_BAR_SECTION;
		counterclockwise_turns = 2u +
			((symbol - V34_PHASE3_S_SYMBOLS) & 1u);
	}
	if (v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0,
		    (uint8_t)((4u - counterclockwise_turns) & 3u),
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT;
	next.emitted_symbols++;
	*sequence = next;
	*point = next_point;
	*section = next_section;
	return V34_PHASE3_S_SEQUENCE_OK;
}
