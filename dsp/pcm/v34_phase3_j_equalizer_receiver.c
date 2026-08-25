// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_j_equalizer_receiver.h"

#include "v34_constellation.h"

#include <math.h>
#include <stddef.h>

bool v34_phase3_j_handoff_is_valid(
	const struct v34_phase3_j_handoff *handoff)
{
	return handoff != NULL &&
	       v34_equalizer_handoff_is_valid(&handoff->equalizer) &&
	       v34_phase3_j_detection_is_valid(&handoff->detection);
}

bool v34_phase3_j_equalizer_receiver_is_valid(
	const struct v34_phase3_j_equalizer_receiver *receiver)
{
	return receiver != NULL &&
	       qam_equalizer_is_valid(&receiver->equalizer) &&
	       !receiver->equalizer.can_adapt &&
	       v34_phase3_j_receiver_is_valid(&receiver->detector) &&
	       isfinite(receiver->adaptation) && receiver->adaptation >= 0.0 &&
	       receiver->adaptation <= 1.0;
}

bool v34_phase3_j_equalizer_receiver_is_complete(
	const struct v34_phase3_j_equalizer_receiver *receiver)
{
	return v34_phase3_j_equalizer_receiver_is_valid(receiver) &&
	       v34_phase3_j_receiver_is_complete(&receiver->detector);
}

enum v34_phase3_j_equalizer_receiver_result
v34_phase3_j_equalizer_receiver_init(
	struct v34_phase3_j_equalizer_receiver *receiver,
	const struct v34_equalizer_handoff *pp_handoff,
	enum v34_station_role transmitting_role, double adaptation)
{
	struct v34_phase3_j_equalizer_receiver initialized = {
		.adaptation = adaptation,
	};

	if (receiver == NULL ||
	    !v34_equalizer_handoff_is_valid(pp_handoff) ||
	    !isfinite(adaptation) || adaptation < 0.0 || adaptation > 1.0 ||
	    v34_phase3_j_receiver_init(
		    &initialized.detector, transmitting_role) !=
		    V34_PHASE3_J_RECEIVER_OK)
		return V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	initialized.equalizer = pp_handoff->equalizer;
	if (!v34_phase3_j_equalizer_receiver_is_valid(&initialized))
		return V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_J_EQUALIZER_RECEIVER_OK;
}

static enum v34_phase3_j_equalizer_receiver_result detector_result(
	enum v34_phase3_j_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_J_RECEIVER_OK:
		return V34_PHASE3_J_EQUALIZER_RECEIVER_OK;
	case V34_PHASE3_J_RECEIVER_DETECTED:
		return V34_PHASE3_J_EQUALIZER_RECEIVER_DETECTED;
	case V34_PHASE3_J_RECEIVER_COMPLETE:
		return V34_PHASE3_J_EQUALIZER_RECEIVER_COMPLETE;
	case V34_PHASE3_J_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE3_J_EQUALIZER_RECEIVER_OUT_OF_RANGE;
	case V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
}

enum v34_phase3_j_equalizer_receiver_result
v34_phase3_j_equalizer_receive_point(
	struct v34_phase3_j_equalizer_receiver *receiver,
	const struct qam_point *received)
{
	struct v34_phase3_j_equalizer_receiver next;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point target;
	enum v34_phase3_j_equalizer_receiver_result result;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	bool ready;

	if (!v34_phase3_j_equalizer_receiver_is_valid(receiver) ||
	    !point_is_finite(received))
		return V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (v34_phase3_j_equalizer_receiver_is_complete(receiver))
		return V34_PHASE3_J_EQUALIZER_RECEIVER_COMPLETE;
	next = *receiver;
	if (qam_equalizer_observe(
		    &next.equalizer, received, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready ||
	    v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT, &observation.output,
		    &quarter_point, &clockwise_quarter_turns) !=
		    V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    V34_CONSTELLATION_4_POINT, quarter_point,
		    clockwise_quarter_turns, &target) != V34_CONSTELLATION_OK ||
	    qam_equalizer_adapt_observation(
		    &next.equalizer, &observation, &target,
		    next.adaptation) != QAM_EQUALIZER_OK)
		return V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	result = detector_result(v34_phase3_j_receiver_receive(
		&next.detector, quarter_point, clockwise_quarter_turns));
	if (result == V34_PHASE3_J_EQUALIZER_RECEIVER_OUT_OF_RANGE)
		return result;
	if (result != V34_PHASE3_J_EQUALIZER_RECEIVER_OK &&
	    result != V34_PHASE3_J_EQUALIZER_RECEIVER_DETECTED)
		return V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	if (!v34_phase3_j_equalizer_receiver_is_valid(&next))
		return V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return result;
}

enum v34_phase3_j_equalizer_receiver_result
v34_phase3_j_equalizer_receiver_handoff(
	const struct v34_phase3_j_equalizer_receiver *receiver,
	struct v34_phase3_j_handoff *handoff)
{
	struct v34_phase3_j_handoff generated;

	if (!v34_phase3_j_equalizer_receiver_is_valid(receiver) ||
	    handoff == NULL)
		return V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase3_j_equalizer_receiver_is_complete(receiver))
		return V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_STATE;
	generated.equalizer.equalizer = receiver->equalizer;
	if (!v34_phase3_j_receiver_get_detection(
		    &receiver->detector, &generated.detection) ||
	    !v34_phase3_j_handoff_is_valid(&generated))
		return V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_STATE;
	*handoff = generated;
	return V34_PHASE3_J_EQUALIZER_RECEIVER_OK;
}
