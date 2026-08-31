// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_line_tx.h"

#include "v90_phase3.h"

#include <stddef.h>

bool v90_phase3_return_line_tx_is_valid(
	const struct v90_phase3_return_line_tx *tx)
{
	return tx != NULL && v90_phase3_training_line_is_valid(&tx->line) &&
	       v90_phase3_return_tx_is_valid(&tx->points);
}

bool v90_phase3_return_line_tx_is_handoff_ready(
	const struct v90_phase3_return_line_tx *tx)
{
	return v90_phase3_return_line_tx_is_valid(tx) &&
	       v90_phase3_return_tx_is_complete(&tx->points) &&
	       v34_training_line_needs_point(&tx->line);
}

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_init(
	struct v90_phase3_return_line_tx *tx,
	const struct v34_training_line *line)
{
	struct v90_phase3_return_line_tx initialized = { 0 };

	if (tx == NULL || !v90_phase3_training_line_is_valid(line) ||
	    !v34_training_line_needs_point(line))
		return V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT;
	initialized.line = *line;
	if (v90_phase3_return_tx_init(&initialized.points) !=
		    V90_PHASE3_RETURN_TX_OK ||
	    !v90_phase3_return_line_tx_is_valid(&initialized))
		return V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V90_PHASE3_RETURN_LINE_TX_OK;
}

static enum v90_phase3_return_line_tx_result control_result(
	struct v90_phase3_return_line_tx *tx,
	enum v90_phase3_return_tx_result result)
{
	if (result == V90_PHASE3_RETURN_TX_INVALID_STATE)
		return V90_PHASE3_RETURN_LINE_TX_INVALID_STATE;
	if (result != V90_PHASE3_RETURN_TX_OK ||
	    !v90_phase3_return_line_tx_is_valid(tx))
		return V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR;
	return V90_PHASE3_RETURN_LINE_TX_OK;
}

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_start_s(
	struct v90_phase3_return_line_tx *tx)
{
	struct v90_phase3_return_line_tx next;
	enum v90_phase3_return_line_tx_result result;

	if (!v90_phase3_return_line_tx_is_valid(tx))
		return V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT;
	next = *tx;
	result = control_result(
		&next, v90_phase3_return_tx_start_s(&next.points));
	if (result == V90_PHASE3_RETURN_LINE_TX_OK)
		*tx = next;
	return result;
}

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_finish_s(
	struct v90_phase3_return_line_tx *tx)
{
	struct v90_phase3_return_line_tx next;
	enum v90_phase3_return_line_tx_result result;

	if (!v90_phase3_return_line_tx_is_valid(tx))
		return V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT;
	next = *tx;
	result = control_result(
		&next, v90_phase3_return_tx_finish_s(&next.points));
	if (result == V90_PHASE3_RETURN_LINE_TX_OK)
		*tx = next;
	return result;
}

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_sample(
	struct v90_phase3_return_line_tx *tx, int16_t *sample)
{
	struct v90_phase3_return_line_tx next;
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	int16_t generated;
	bool accepting;
	bool accepted;

	if (!v90_phase3_return_line_tx_is_valid(tx) || sample == NULL)
		return V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT;
	if (v90_phase3_return_line_tx_is_handoff_ready(tx))
		return V90_PHASE3_RETURN_LINE_TX_COMPLETE;
	next = *tx;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		enum v90_phase3_return_tx_result point_result =
			v90_phase3_return_tx_next(&next.points, &point);

		if (point_result == V90_PHASE3_RETURN_TX_OUT_OF_RANGE)
			return V90_PHASE3_RETURN_LINE_TX_OUT_OF_RANGE;
		if (point_result != V90_PHASE3_RETURN_TX_OK)
			return V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR;
		supplied = &point;
	}
	if (v34_training_line_sample(
		    &next.line, supplied, &generated, &accepted) !=
		    V34_TRAINING_LINE_OK ||
	    accepted != accepting ||
	    !v90_phase3_return_line_tx_is_valid(&next))
		return V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR;
	*tx = next;
	*sample = generated;
	return V90_PHASE3_RETURN_LINE_TX_OK;
}

enum v90_phase3_return_line_tx_result v90_phase3_return_line_tx_handoff(
	const struct v90_phase3_return_line_tx *tx,
	struct v34_training_line *line)
{
	if (!v90_phase3_return_line_tx_is_valid(tx) || line == NULL)
		return V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT;
	if (!v90_phase3_return_line_tx_is_handoff_ready(tx))
		return V90_PHASE3_RETURN_LINE_TX_INVALID_STATE;
	*line = tx->line;
	return V90_PHASE3_RETURN_LINE_TX_OK;
}

const char *v90_phase3_return_line_tx_result_name(
	enum v90_phase3_return_line_tx_result result)
{
	switch (result) {
	case V90_PHASE3_RETURN_LINE_TX_OK:
		return "ok";
	case V90_PHASE3_RETURN_LINE_TX_COMPLETE:
		return "complete";
	case V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_RETURN_LINE_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_RETURN_LINE_TX_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
