// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_line_tx.h"

#include "memory_region.h"
#include "v90_phase3.h"

#include <stddef.h>

bool v90_phase4_cp_line_tx_is_valid(
	const struct v90_phase4_cp_line_tx *tx)
{
	return tx != NULL && v90_phase3_training_line_is_valid(&tx->line) &&
	       v90_phase4_cp_tx_is_valid(&tx->symbols) &&
	       tx->symbols.role == tx->line.profile.local_role;
}

bool v90_phase4_cp_line_tx_record_is_complete(
	const struct v90_phase4_cp_line_tx *tx)
{
	return v90_phase4_cp_line_tx_is_valid(tx) &&
	       tx->symbols.bit_index == tx->symbols.frame_bit_count;
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

	if (tx == NULL || !v90_phase3_training_line_is_valid(line) ||
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

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_resume_online(
	struct v90_phase4_cp_line_tx *tx,
	const struct v34_training_line *online_line,
	const struct v90_phase4_cp_tx *control_symbols,
	enum v34_constellation_size constellation,
	const struct v90_cp_parameters *parameters)
{
	struct v90_phase4_cp_line_tx resumed = { 0 };
	enum v90_phase4_cp_line_tx_result result;

	if (tx == NULL || online_line == NULL || control_symbols == NULL ||
	    parameters == NULL ||
	    memory_regions_overlap(
		    tx, sizeof(*tx), online_line, sizeof(*online_line)) ||
	    memory_regions_overlap(
		    tx, sizeof(*tx), control_symbols,
		    sizeof(*control_symbols)) ||
	    !v34_constellation_size_is_valid(constellation) ||
	    !v90_phase3_training_line_is_valid(online_line) ||
	    !v90_phase4_cp_tx_is_valid(control_symbols) ||
	    control_symbols->role != online_line->profile.local_role)
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	if (!v34_training_line_needs_point(online_line) ||
	    control_symbols->bit_index != control_symbols->frame_bit_count)
		return V90_PHASE4_CP_LINE_TX_INVALID_STATE;
	if (!v90_cp_parameters_are_valid(parameters) ||
	    parameters->kind != V90_CP_DATA)
		return V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS;

	resumed.line = *online_line;
	resumed.symbols = *control_symbols;
	resumed.symbols.constellation = constellation;
	result = symbol_result(v90_phase4_cp_tx_start_next_record(
		&resumed.symbols, parameters));
	if (result != V90_PHASE4_CP_LINE_TX_OK)
		return result;
	if (!v90_phase4_cp_line_tx_is_valid(&resumed))
		return V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR;
	*tx = resumed;
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
	accepting = v34_training_line_needs_point(&tx->line);
	if (accepting && tx->symbols.bit_index == tx->symbols.frame_bit_count)
		return V90_PHASE4_CP_LINE_TX_COMPLETE;
	next = *tx;
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

enum v90_phase4_cp_line_tx_result v90_phase4_cp_line_tx_scr_sample(
	struct v90_phase4_cp_line_tx *tx, bool stop_at_boundary,
	int16_t *sample)
{
	struct v90_phase4_cp_line_tx next;
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	int16_t generated;
	bool accepting;
	bool accepted;

	if (!v90_phase4_cp_line_tx_is_valid(tx) || sample == NULL)
		return V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT;
	if (tx->symbols.bit_index != tx->symbols.frame_bit_count)
		return V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS;
	if (stop_at_boundary && v34_training_line_needs_point(&tx->line))
		return V90_PHASE4_CP_LINE_TX_COMPLETE;
	next = *tx;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		if (v90_phase4_cp_tx_scr_next(
			    &next.symbols, &point, &quarter_point,
			    &clockwise_quarter_turns) !=
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
	if (tx->symbols.bit_index != tx->symbols.frame_bit_count ||
	    !v34_training_line_needs_point(&tx->line))
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
