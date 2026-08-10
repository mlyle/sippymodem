// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase3_s_sequence.h"

#include <stddef.h>

static const double V34_POINT_ZERO_COORDINATE =
	0.70710678118654752440084436210485;

_Static_assert(
	SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS <= UINT16_MAX,
	"V.34 S/S-bar sequence counter must hold the complete sequence");

static struct sippymodem_qam_point rotated_point_zero(
	unsigned int counterclockwise_quarter_turns)
{
	double coordinate = V34_POINT_ZERO_COORDINATE;

	switch (counterclockwise_quarter_turns & 3u) {
	case 0:
		return (struct sippymodem_qam_point){ coordinate, coordinate };
	case 1:
		return (struct sippymodem_qam_point){ -coordinate, coordinate };
	case 2:
		return (struct sippymodem_qam_point){ -coordinate, -coordinate };
	case 3:
		return (struct sippymodem_qam_point){ coordinate, -coordinate };
	}
	return (struct sippymodem_qam_point){ 0 };
}

bool sippymodem_v34_phase3_s_sequence_is_valid(
	const struct sippymodem_v34_phase3_s_sequence *sequence)
{
	return sequence != NULL &&
	       sequence->emitted_symbols <=
		       SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS;
}

bool sippymodem_v34_phase3_s_sequence_is_complete(
	const struct sippymodem_v34_phase3_s_sequence *sequence)
{
	return sippymodem_v34_phase3_s_sequence_is_valid(sequence) &&
	       sequence->emitted_symbols ==
		       SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS;
}

enum sippymodem_v34_phase3_s_sequence_result
sippymodem_v34_phase3_s_sequence_init(
	struct sippymodem_v34_phase3_s_sequence *sequence)
{
	if (sequence == NULL)
		return SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT;
	*sequence = (struct sippymodem_v34_phase3_s_sequence){ 0 };
	return SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK;
}

enum sippymodem_v34_phase3_s_sequence_result
sippymodem_v34_phase3_s_sequence_next(
	struct sippymodem_v34_phase3_s_sequence *sequence,
	struct sippymodem_qam_point *point,
	enum sippymodem_v34_phase3_s_section *section)
{
	struct sippymodem_qam_point next_point;
	enum sippymodem_v34_phase3_s_section next_section;
	unsigned int quarter_turns;
	uint16_t symbol;

	if (!sippymodem_v34_phase3_s_sequence_is_valid(sequence) ||
	    point == NULL || section == NULL)
		return SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT;
	if (sippymodem_v34_phase3_s_sequence_is_complete(sequence))
		return SIPPYMODEM_V34_PHASE3_S_SEQUENCE_COMPLETE;
	symbol = sequence->emitted_symbols;
	if (symbol < SIPPYMODEM_V34_PHASE3_S_SYMBOLS) {
		next_section = SIPPYMODEM_V34_PHASE3_S_SECTION;
		quarter_turns = symbol & 1u;
	} else {
		next_section = SIPPYMODEM_V34_PHASE3_S_BAR_SECTION;
		quarter_turns = 2u +
			((symbol - SIPPYMODEM_V34_PHASE3_S_SYMBOLS) & 1u);
	}
	next_point = rotated_point_zero(quarter_turns);
	sequence->emitted_symbols++;
	*point = next_point;
	*section = next_section;
	return SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK;
}

const char *sippymodem_v34_phase3_s_sequence_result_name(
	enum sippymodem_v34_phase3_s_sequence_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE3_S_SEQUENCE_COMPLETE:
		return "complete";
	case SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
