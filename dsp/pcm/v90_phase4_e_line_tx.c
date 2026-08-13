// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_e_line_tx.h"

#include "v90_phase3.h"

#include <stddef.h>

static bool line_is_analog(const struct v34_training_line *line)
{
	return v34_training_line_is_valid(line) &&
	       line->carrier.channel.sample_rate == V90_PHASE3_SAMPLE_RATE &&
	       line->profile.local_role == V34_ANSWERING_STATION &&
	       line->profile.md_symbol_count == 0;
}

bool v90_phase4_e_line_tx_is_valid(
	const struct v90_phase4_e_line_tx *transmitter)
{
	return transmitter != NULL && line_is_analog(&transmitter->line) &&
	       v34_phase4_e_sequence_is_valid(&transmitter->sequence) &&
	       transmitter->sequence.role ==
		       transmitter->line.profile.local_role;
}

bool v90_phase4_e_line_tx_is_complete(
	const struct v90_phase4_e_line_tx *transmitter)
{
	return v90_phase4_e_line_tx_is_valid(transmitter) &&
	       v34_phase4_e_sequence_is_complete(&transmitter->sequence) &&
	       v34_training_line_needs_point(&transmitter->line);
}

enum v90_phase4_e_line_tx_result v90_phase4_e_line_tx_init(
	struct v90_phase4_e_line_tx *transmitter,
	const struct v90_phase4_cp_line_tx *completed_cp)
{
	struct v90_phase4_e_line_tx initialized = { 0 };

	if (transmitter == NULL || completed_cp == NULL)
		return V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT;
	if (!v90_phase4_cp_line_tx_is_handoff_ready(completed_cp))
		return V90_PHASE4_E_LINE_TX_INVALID_STATE;
	if (v90_phase4_cp_line_tx_handoff(
		    completed_cp, &initialized.line) !=
		    V90_PHASE4_CP_LINE_TX_OK ||
	    v34_phase4_e_sequence_init(
		    &initialized.sequence,
		    &completed_cp->symbols.scrambler,
		    completed_cp->symbols.role,
		    completed_cp->symbols.constellation,
		    completed_cp->symbols
			    .previous_clockwise_quarter_turns) !=
		    V34_PHASE4_E_SEQUENCE_OK ||
	    !v90_phase4_e_line_tx_is_valid(&initialized))
		return V90_PHASE4_E_LINE_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return V90_PHASE4_E_LINE_TX_OK;
}

static enum v90_phase4_e_line_tx_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V90_PHASE4_E_LINE_TX_OK;
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return V90_PHASE4_E_LINE_TX_OUT_OF_RANGE;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V90_PHASE4_E_LINE_TX_CLIPPING_RISK;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V90_PHASE4_E_LINE_TX_INTERNAL_ERROR;
	}
	return V90_PHASE4_E_LINE_TX_INTERNAL_ERROR;
}

enum v90_phase4_e_line_tx_result v90_phase4_e_line_tx_sample(
	struct v90_phase4_e_line_tx *transmitter, int16_t *sample)
{
	struct v90_phase4_e_line_tx next;
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	int16_t generated;
	bool accepting;
	bool accepted;
	enum v90_phase4_e_line_tx_result result;

	if (!v90_phase4_e_line_tx_is_valid(transmitter) || sample == NULL)
		return V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT;
	if (v90_phase4_e_line_tx_is_complete(transmitter))
		return V90_PHASE4_E_LINE_TX_COMPLETE;
	next = *transmitter;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		if (v34_phase4_e_sequence_next(
			    &next.sequence, &point, &quarter_point,
			    &clockwise_quarter_turns) !=
		    V34_PHASE4_E_SEQUENCE_OK)
			return V90_PHASE4_E_LINE_TX_INTERNAL_ERROR;
		supplied = &point;
	}
	result = line_result(v34_training_line_sample(
		&next.line, supplied, &generated, &accepted));
	if (result != V90_PHASE4_E_LINE_TX_OK)
		return result;
	if (accepted != accepting ||
	    !v90_phase4_e_line_tx_is_valid(&next))
		return V90_PHASE4_E_LINE_TX_INTERNAL_ERROR;
	*transmitter = next;
	*sample = generated;
	return V90_PHASE4_E_LINE_TX_OK;
}

enum v90_phase4_e_line_tx_result v90_phase4_e_line_tx_handoff(
	const struct v90_phase4_e_line_tx *transmitter,
	struct v34_training_line *line)
{
	if (!v90_phase4_e_line_tx_is_valid(transmitter) || line == NULL)
		return V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT;
	if (!v90_phase4_e_line_tx_is_complete(transmitter))
		return V90_PHASE4_E_LINE_TX_INVALID_STATE;
	*line = transmitter->line;
	return V90_PHASE4_E_LINE_TX_OK;
}

const char *v90_phase4_e_line_tx_result_name(
	enum v90_phase4_e_line_tx_result result)
{
	switch (result) {
	case V90_PHASE4_E_LINE_TX_OK:
		return "ok";
	case V90_PHASE4_E_LINE_TX_COMPLETE:
		return "complete";
	case V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_E_LINE_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_E_LINE_TX_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE4_E_LINE_TX_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE4_E_LINE_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
