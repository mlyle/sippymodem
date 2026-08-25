// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_pp_receiver.h"

#include <math.h>
#include <stddef.h>

static bool sequence_counts_are_valid(
	const struct v34_phase3_pp_receiver *receiver)
{
	unsigned int received = receiver->received_targets.emitted_symbols;
	unsigned int adapted = receiver->adaptation_targets.emitted_symbols;
	unsigned int expected_adapted;

	if (adapted > received ||
	    received - adapted > V34_EQUALIZER_DECISION_DELAY)
		return false;
	if (received < V34_PHASE3_PP_SEQUENCE_SYMBOLS) {
		expected_adapted = received > V34_EQUALIZER_DECISION_DELAY ?
				   received - V34_EQUALIZER_DECISION_DELAY :
				   0;
		return adapted == expected_adapted;
	}
	return adapted >=
	       V34_PHASE3_PP_SEQUENCE_SYMBOLS -
		       V34_EQUALIZER_DECISION_DELAY;
}

bool v34_phase3_pp_receiver_is_valid(
	const struct v34_phase3_pp_receiver *receiver)
{
	if (receiver == NULL || !isfinite(receiver->adaptation) ||
	    receiver->adaptation < 0.0 || receiver->adaptation > 1.0 ||
	    !v34_phase3_pp_sequence_is_valid(&receiver->received_targets) ||
	    !v34_phase3_pp_sequence_is_valid(&receiver->adaptation_targets) ||
	    !sequence_counts_are_valid(receiver))
		return false;
	if (!receiver->seeded)
		return receiver->received_targets.emitted_symbols == 0 &&
		       receiver->adaptation_targets.emitted_symbols == 0;
	return receiver->received_targets.emitted_symbols != 0 &&
	       qam_equalizer_is_valid(&receiver->equalizer) &&
	       !receiver->equalizer.can_adapt;
}

bool v34_phase3_pp_receiver_is_complete(
	const struct v34_phase3_pp_receiver *receiver)
{
	return v34_phase3_pp_receiver_is_valid(receiver) && receiver->seeded &&
	       v34_phase3_pp_sequence_is_complete(
		       &receiver->received_targets) &&
	       v34_phase3_pp_sequence_is_complete(
		       &receiver->adaptation_targets);
}

enum v34_phase3_pp_receiver_result v34_phase3_pp_receiver_init(
	struct v34_phase3_pp_receiver *receiver, double adaptation)
{
	struct v34_phase3_pp_receiver initialized = {
		.adaptation = adaptation,
	};

	if (receiver == NULL || !isfinite(adaptation) || adaptation < 0.0 ||
	    adaptation > 1.0 ||
	    v34_phase3_pp_sequence_init(&initialized.received_targets) !=
		    V34_PHASE3_PP_SEQUENCE_OK ||
	    v34_phase3_pp_sequence_init(&initialized.adaptation_targets) !=
		    V34_PHASE3_PP_SEQUENCE_OK ||
	    !v34_phase3_pp_receiver_is_valid(&initialized))
		return V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_PP_RECEIVER_OK;
}

static bool seed_equalizer(
	struct v34_phase3_pp_receiver *receiver,
	const struct qam_point *received, const struct qam_point *target)
{
	if (qam_equalizer_init_geometry(
		    &receiver->equalizer, received, target,
		    V34_EQUALIZER_TAP_COUNT,
		    V34_EQUALIZER_DECISION_DELAY) != QAM_EQUALIZER_OK)
		return false;
	receiver->seeded = true;
	return true;
}

enum v34_phase3_pp_receiver_result v34_phase3_pp_receive_point(
	struct v34_phase3_pp_receiver *receiver,
	const struct qam_point *received)
{
	struct v34_phase3_pp_receiver next;
	struct qam_point target;
	struct qam_point output;
	bool ready;

	if (!v34_phase3_pp_receiver_is_valid(receiver) ||
	    !point_is_finite(received))
		return V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT;
	if (v34_phase3_pp_receiver_is_complete(receiver))
		return V34_PHASE3_PP_RECEIVER_COMPLETE;
	next = *receiver;
	if (!v34_phase3_pp_sequence_is_complete(&next.received_targets)) {
		if (v34_phase3_pp_sequence_next(
			    &next.received_targets, &target) !=
		    V34_PHASE3_PP_SEQUENCE_OK)
			return V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE;
		if (!next.seeded && !seed_equalizer(&next, received, &target))
			return V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE;
	}
	if (qam_equalizer_push(
		    &next.equalizer, received, &output, &ready) !=
	    QAM_EQUALIZER_OK)
		return V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE;
	if (ready) {
		if (v34_phase3_pp_sequence_next(
			    &next.adaptation_targets, &target) !=
			    V34_PHASE3_PP_SEQUENCE_OK ||
		    qam_equalizer_adapt(
			    &next.equalizer, &target, next.adaptation) !=
			    QAM_EQUALIZER_OK)
			return V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE;
	}
	if (!v34_phase3_pp_receiver_is_valid(&next))
		return V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return V34_PHASE3_PP_RECEIVER_OK;
}

enum v34_phase3_pp_receiver_result v34_phase3_pp_receiver_handoff(
	const struct v34_phase3_pp_receiver *receiver,
	struct v34_equalizer_handoff *handoff)
{
	struct v34_equalizer_handoff generated;

	if (!v34_phase3_pp_receiver_is_valid(receiver) || handoff == NULL)
		return V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase3_pp_receiver_is_complete(receiver))
		return V34_PHASE3_PP_RECEIVER_INVALID_STATE;
	generated.equalizer = receiver->equalizer;
	if (!v34_equalizer_handoff_is_valid(&generated))
		return V34_PHASE3_PP_RECEIVER_INVALID_STATE;
	*handoff = generated;
	return V34_PHASE3_PP_RECEIVER_OK;
}
