// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_pp_sequence.h"

#include <stddef.h>

static const double V34_COSINE_PI_OVER_SIX =
	0.86602540378443864676372317075294;
static const double V34_SINE_PI_OVER_SIX = 0.5;

_Static_assert(
	SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS <= UINT16_MAX,
	"V.34 PP sequence counter must hold the complete sequence");

static unsigned int pp_phase(uint16_t symbol)
{
	unsigned int period_symbol =
		symbol % SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS;
	unsigned int k = period_symbol / 4u;
	unsigned int i = period_symbol % 4u;
	unsigned int phase = k * i;

	if (k % 3u == 1u)
		phase += 4u;
	return phase % 12u;
}

static struct sippymodem_qam_point unit_circle_point(unsigned int phase)
{
	struct sippymodem_qam_point point = { 1.0, 0.0 };

	/* Equation 10-1 uses only twelfth roots of unity. Generate the selected
	 * root with the same fixed 30-degree rotation instead of retaining the
	 * recommendation's 48-point period as a coefficient table. */
	for (unsigned int turn = 0; turn < phase; ++turn) {
		double in_phase =
			point.in_phase * V34_COSINE_PI_OVER_SIX -
			point.quadrature * V34_SINE_PI_OVER_SIX;
		double quadrature =
			point.in_phase * V34_SINE_PI_OVER_SIX +
			point.quadrature * V34_COSINE_PI_OVER_SIX;

		point.in_phase = in_phase;
		point.quadrature = quadrature;
	}
	return point;
}

bool sippymodem_v34_phase3_pp_sequence_is_valid(
	const struct sippymodem_v34_phase3_pp_sequence *sequence)
{
	return sequence != NULL &&
	       sequence->emitted_symbols <=
		       SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS;
}

bool sippymodem_v34_phase3_pp_sequence_is_complete(
	const struct sippymodem_v34_phase3_pp_sequence *sequence)
{
	return sippymodem_v34_phase3_pp_sequence_is_valid(sequence) &&
	       sequence->emitted_symbols ==
		       SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS;
}

enum sippymodem_v34_phase3_pp_sequence_result
sippymodem_v34_phase3_pp_sequence_init(
	struct sippymodem_v34_phase3_pp_sequence *sequence)
{
	if (sequence == NULL)
		return SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT;
	*sequence = (struct sippymodem_v34_phase3_pp_sequence){ 0 };
	return SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK;
}

enum sippymodem_v34_phase3_pp_sequence_result
sippymodem_v34_phase3_pp_sequence_next(
	struct sippymodem_v34_phase3_pp_sequence *sequence,
	struct sippymodem_qam_point *point)
{
	struct sippymodem_qam_point next_point;

	if (!sippymodem_v34_phase3_pp_sequence_is_valid(sequence) ||
	    point == NULL)
		return SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT;
	if (sippymodem_v34_phase3_pp_sequence_is_complete(sequence))
		return SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_COMPLETE;
	next_point = unit_circle_point(pp_phase(sequence->emitted_symbols));
	sequence->emitted_symbols++;
	*point = next_point;
	return SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK;
}

const char *sippymodem_v34_phase3_pp_sequence_result_name(
	enum sippymodem_v34_phase3_pp_sequence_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_COMPLETE:
		return "complete";
	case SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
