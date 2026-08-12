// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_line_tx.h"

#include "v90_phase3.h"

#include <stddef.h>

static bool line_is_analog(const struct v34_training_line *line)
{
	return v34_training_line_is_valid(line) &&
	       line->carrier.channel.sample_rate == V90_PHASE3_SAMPLE_RATE &&
	       line->profile.local_role == V34_ANSWERING_STATION &&
	       line->profile.md_symbol_count == 0;
}

bool v90_phase4_cp_line_tx_is_valid(
	const struct v90_phase4_cp_line_tx *tx)
{
	return tx != NULL && line_is_analog(&tx->line) &&
	       v90_phase4_cp_tx_is_valid(&tx->symbols) &&
	       tx->symbols.role == tx->line.profile.local_role;
}

bool v90_phase4_cp_line_tx_record_is_complete(
	const struct v90_phase4_cp_line_tx *tx)
{
	return v90_phase4_cp_line_tx_is_valid(tx) &&
	       v90_phase4_cp_tx_record_is_complete(&tx->symbols);
}

bool v90_phase4_cp_line_tx_is_handoff_ready(
	const struct v90_phase4_cp_line_tx *tx)
{
	return v90_phase4_cp_line_tx_record_is_complete(tx) &&
	       v34_training_line_needs_point(&tx->line);
}

static enum v90_phase4_cp_line_tx_result symbol_result(
	enum v90_phase4_cp_tx_result result)
{
	switch (result) {
	case V90_PHASE4_CP_TX_OK:
		return V90_PHASE4_CP_LINE_TX_OK;
	case V90_PHASE4_CP_TX_RECORD_COMPLETE:
		return V90_PHASE4_CP_LINE_TX_COMPLETE;
	case V90_PHASE4_CP_TX_RECORD_IN_PROGRESS:
		return V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS;
	case V90_PHASE4_CP_TX_INVALID_ARGUMENT:
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	case V90_PHASE4_CP_TX_INVALID_PARAMETERS:
		return V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS;
	}
	return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
}

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_init(
	struct v90_phase4_cp_line_tx *tx,
	const struct v34_training_line *line,
	enum v34_constellation_size constellation,
	const struct v90_cp_parameters *training_parameters)
{
	struct v90_phase4_cp_line_tx initialized = { 0 };
	enum v90_phase4_cp_line_tx_result result;

	if (tx == NULL || !line_is_analog(line) ||
	    !v34_training_line_needs_point(line))
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	initialized.line = *line;
	result = symbol_result(v90_phase4_cp_tx_init(
		&initialized.symbols, line->profile.local_role,
		constellation, training_parameters));
	if (result != V90_PHASE4_CP_LINE_TX_OK)
		return result;
	if (!v90_phase4_cp_line_tx_is_valid(&initialized))
		return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V90_PHASE4_CP_LINE_TX_OK;
}

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_start_next_record(
	struct v90_phase4_cp_line_tx *tx,
	const struct v90_cp_parameters *parameters)
{
	struct v90_phase4_cp_line_tx next;
	enum v90_phase4_cp_line_tx_result result;

	if (!v90_phase4_cp_line_tx_is_valid(tx))
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	next = *tx;
	result = symbol_result(v90_phase4_cp_tx_start_next_record(
		&next.symbols, parameters));
	if (result != V90_PHASE4_CP_LINE_TX_OK)
		return result;
	if (!v90_phase4_cp_line_tx_is_valid(&next))
		return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
	*tx = next;
	return V90_PHASE4_CP_LINE_TX_OK;
}

static enum v90_phase4_cp_line_tx_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V90_PHASE4_CP_LINE_TX_OK;
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return V90_PHASE4_CP_LINE_TX_OUT_OF_RANGE;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V90_PHASE4_CP_LINE_TX_CLIPPING_RISK;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
	}
	return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
}

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_sample(
	struct v90_phase4_cp_line_tx *tx, int16_t *sample)
{
	struct v90_phase4_cp_line_tx next;
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	int16_t generated;
	bool accepting;
	bool accepted;

	if (!v90_phase4_cp_line_tx_is_valid(tx) || sample == NULL)
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	if (v90_phase4_cp_line_tx_is_handoff_ready(tx))
		return V90_PHASE4_CP_LINE_TX_COMPLETE;
	next = *tx;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;
		bool record_complete;

		if (v90_phase4_cp_tx_next(
			    &next.symbols, &point, &quarter_point,
			    &clockwise_quarter_turns, &record_complete) !=
		    V90_PHASE4_CP_TX_OK)
			return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
		supplied = &point;
	}
	{
		enum v90_phase4_cp_line_tx_result result = line_result(
			v34_training_line_sample(
				&next.line, supplied, &generated, &accepted));

		if (result != V90_PHASE4_CP_LINE_TX_OK)
			return result;
	}
	if (accepted != accepting ||
	    !v90_phase4_cp_line_tx_is_valid(&next))
		return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
	*tx = next;
	*sample = generated;
	return V90_PHASE4_CP_LINE_TX_OK;
}

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_handoff(
	const struct v90_phase4_cp_line_tx *tx,
	struct v34_training_line *line)
{
	if (!v90_phase4_cp_line_tx_is_valid(tx) || line == NULL)
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	if (!v90_phase4_cp_line_tx_is_handoff_ready(tx))
		return V90_PHASE4_CP_LINE_TX_INVALID_STATE;
	*line = tx->line;
	return V90_PHASE4_CP_LINE_TX_OK;
}

const char *v90_phase4_cp_line_tx_result_name(
	enum v90_phase4_cp_line_tx_result result)
{
	switch (result) {
	case V90_PHASE4_CP_LINE_TX_OK:
		return "ok";
	case V90_PHASE4_CP_LINE_TX_COMPLETE:
		return "complete";
	case V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS:
		return "record in progress";
	case V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_PHASE4_CP_LINE_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_CP_LINE_TX_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE4_CP_LINE_TX_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
