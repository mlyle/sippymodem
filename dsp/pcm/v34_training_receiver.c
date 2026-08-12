// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_receiver.h"

#include <math.h>
#include <stddef.h>

bool v34_training_receiver_configuration_is_valid(
	const struct v34_training_receiver_configuration *configuration)
{
	return configuration != NULL &&
	       v34_phase3_line_receiver_configuration_is_valid(
		       &configuration->phase3) &&
	       isfinite(configuration->phase4_equalizer_adaptation) &&
	       configuration->phase4_equalizer_adaptation >= 0.0 &&
	       configuration->phase4_equalizer_adaptation <= 1.0;
}

static bool phase3_configuration_matches(
	const struct v34_training_receiver *receiver)
{
	const struct v34_phase3_receiver_configuration *active =
		&receiver->state.phase3.points.configuration;
	const struct v34_phase3_line_receiver_configuration *configured =
		&receiver->configuration.phase3;

	return active->s_gain_adaptation == configured->s_gain_adaptation &&
	       active->pp_equalizer_adaptation ==
		       configured->pp_equalizer_adaptation &&
	       active->tracking_equalizer_adaptation ==
		       configured->tracking_equalizer_adaptation;
}

bool v34_training_receiver_is_valid(
	const struct v34_training_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_training_receiver_configuration_is_valid(
		    &receiver->configuration))
		return false;
	switch (receiver->stage) {
	case V34_TRAINING_RECEIVER_PHASE3:
		return v34_phase3_line_receiver_is_valid(
			       &receiver->state.phase3) &&
		       phase3_configuration_matches(receiver) &&
		       receiver->failed ==
			       v34_phase3_line_receiver_has_failed(
				       &receiver->state.phase3) &&
		       (receiver->failed ||
			!v34_phase3_receiver_is_complete(
				&receiver->state.phase3.points));
	case V34_TRAINING_RECEIVER_PHASE4:
		return v34_phase4_line_receiver_is_valid(
			       &receiver->state.phase4) &&
		       receiver->state.phase4.control.adaptation ==
			       receiver->configuration
				       .phase4_equalizer_adaptation &&
		       receiver->failed ==
			       v34_phase4_line_receiver_has_failed(
				       &receiver->state.phase4);
	}
	return false;
}

bool v34_training_receiver_has_failed(
	const struct v34_training_receiver *receiver)
{
	return v34_training_receiver_is_valid(receiver) && receiver->failed;
}

bool v34_training_receiver_is_complete(
	const struct v34_training_receiver *receiver)
{
	return v34_training_receiver_is_valid(receiver) && !receiver->failed &&
	       receiver->stage == V34_TRAINING_RECEIVER_PHASE4 &&
	       receiver->state.phase4.control.stage ==
		       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE;
}

enum v34_training_receiver_result v34_training_receiver_init(
	struct v34_training_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	uint32_t sample_rate,
	const struct v34_training_receiver_configuration *configuration)
{
	struct v34_training_receiver initialized = { 0 };
	enum v34_phase3_line_receiver_result result;

	if (receiver == NULL ||
	    !v34_training_receiver_configuration_is_valid(configuration))
		return V34_TRAINING_RECEIVER_INVALID_ARGUMENT;
	initialized.configuration = *configuration;
	initialized.stage = V34_TRAINING_RECEIVER_PHASE3;
	result = v34_phase3_line_receiver_init(
		&initialized.state.phase3, transmitting_profile, sample_rate,
		&configuration->phase3);
	if (result == V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE)
		return V34_TRAINING_RECEIVER_OUT_OF_RANGE;
	if (result != V34_PHASE3_LINE_RECEIVER_OK ||
	    !v34_training_receiver_is_valid(&initialized))
		return V34_TRAINING_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_TRAINING_RECEIVER_OK;
}

enum v34_training_receiver_result v34_training_receiver_continue(
	struct v34_training_receiver *receiver,
	const struct v34_phase3_line_handoff *handoff,
	const struct v34_training_receiver_configuration *configuration)
{
	struct v34_training_receiver initialized = {
		.stage = V34_TRAINING_RECEIVER_PHASE4,
	};

	if (receiver == NULL || !v34_phase3_line_handoff_is_valid(handoff) ||
	    !v34_training_receiver_configuration_is_valid(configuration))
		return V34_TRAINING_RECEIVER_INVALID_ARGUMENT;
	initialized.configuration = *configuration;
	if (v34_phase4_line_receiver_init_from_phase3(
		    &initialized.state.phase4, handoff,
		    configuration->phase4_equalizer_adaptation) !=
		    V34_PHASE4_LINE_RECEIVER_OK ||
	    !v34_training_receiver_is_valid(&initialized))
		return V34_TRAINING_RECEIVER_NUMERIC_FAILURE;
	*receiver = initialized;
	return V34_TRAINING_RECEIVER_OK;
}

static enum v34_training_receiver_result phase3_result(
	enum v34_phase3_line_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_LINE_RECEIVER_OK:
		return V34_TRAINING_RECEIVER_OK;
	case V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED:
		return V34_TRAINING_RECEIVER_TIMING_ACQUIRED;
	case V34_PHASE3_LINE_RECEIVER_S_DETECTED:
		return V34_TRAINING_RECEIVER_S_DETECTED;
	case V34_PHASE3_LINE_RECEIVER_MD_COMPLETE:
		return V34_TRAINING_RECEIVER_MD_COMPLETE;
	case V34_PHASE3_LINE_RECEIVER_REPEATED_S_DETECTED:
		return V34_TRAINING_RECEIVER_REPEATED_S_DETECTED;
	case V34_PHASE3_LINE_RECEIVER_PP_COMPLETE:
		return V34_TRAINING_RECEIVER_PP_COMPLETE;
	case V34_PHASE3_LINE_RECEIVER_J_DETECTED:
		return V34_TRAINING_RECEIVER_J_DETECTED;
	case V34_PHASE3_LINE_RECEIVER_COMPLETE:
		return V34_TRAINING_RECEIVER_COMPLETE;
	case V34_PHASE3_LINE_RECEIVER_FAILED:
		return V34_TRAINING_RECEIVER_FAILED;
	case V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE:
		return V34_TRAINING_RECEIVER_OUT_OF_RANGE;
	case V34_PHASE3_LINE_RECEIVER_INVALID_STATE:
		return V34_TRAINING_RECEIVER_INVALID_STATE;
	case V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_TRAINING_RECEIVER_NUMERIC_FAILURE;
}

static enum v34_training_receiver_result phase4_result(
	enum v34_phase4_line_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_LINE_RECEIVER_OK:
		return V34_TRAINING_RECEIVER_OK;
	case V34_PHASE4_LINE_RECEIVER_SYMBOL:
		return V34_TRAINING_RECEIVER_SYMBOL;
	case V34_PHASE4_LINE_RECEIVER_RECORD_READY:
		return V34_TRAINING_RECEIVER_RECORD_READY;
	case V34_PHASE4_LINE_RECEIVER_E_COMPLETE:
		return V34_TRAINING_RECEIVER_E_COMPLETE;
	case V34_PHASE4_LINE_RECEIVER_COMPLETE:
		return V34_TRAINING_RECEIVER_COMPLETE;
	case V34_PHASE4_LINE_RECEIVER_FAILED:
		return V34_TRAINING_RECEIVER_FAILED;
	case V34_PHASE4_LINE_RECEIVER_INVALID_STATE:
		return V34_TRAINING_RECEIVER_INVALID_STATE;
	case V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH:
		return V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH;
	case V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_TRAINING_RECEIVER_NUMERIC_FAILURE;
}

static bool terminal_result(enum v34_training_receiver_result result)
{
	return result == V34_TRAINING_RECEIVER_NUMERIC_FAILURE ||
	       result == V34_TRAINING_RECEIVER_OUT_OF_RANGE ||
	       result == V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH;
}

static enum v34_training_receiver_result enter_phase4(
	struct v34_training_receiver *receiver)
{
	struct v34_phase3_line_handoff handoff;
	struct v34_phase4_line_receiver phase4;
	struct v34_training_receiver transitioned = {
		.configuration = receiver->configuration,
		.stage = V34_TRAINING_RECEIVER_PHASE4,
	};

	if (v34_phase3_line_receiver_handoff(
		    &receiver->state.phase3, &handoff) !=
		    V34_PHASE3_LINE_RECEIVER_OK ||
	    v34_phase4_line_receiver_init_from_phase3(
		    &phase4, &handoff,
		    receiver->configuration.phase4_equalizer_adaptation) !=
		    V34_PHASE4_LINE_RECEIVER_OK) {
		receiver->state.phase3.failed = true;
		receiver->failed = true;
		return V34_TRAINING_RECEIVER_NUMERIC_FAILURE;
	}
	transitioned.state.phase4 = phase4;
	if (!v34_training_receiver_is_valid(&transitioned)) {
		receiver->state.phase3.failed = true;
		receiver->failed = true;
		return V34_TRAINING_RECEIVER_NUMERIC_FAILURE;
	}
	*receiver = transitioned;
	return V34_TRAINING_RECEIVER_J_DETECTED;
}

static enum v34_training_receiver_result sample_phase3(
	struct v34_training_receiver *receiver, int16_t sample)
{
	enum v34_training_receiver_result result = phase3_result(
		v34_phase3_line_receiver_sample(
			&receiver->state.phase3, sample));

	if (result == V34_TRAINING_RECEIVER_J_DETECTED)
		return enter_phase4(receiver);
	if (terminal_result(result)) {
		receiver->state.phase3.failed = true;
		receiver->failed = true;
	}
	return result;
}

static enum v34_training_receiver_result sample_phase4(
	struct v34_training_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event)
{
	struct v34_mp_receive_event emitted = { 0 };
	enum v34_training_receiver_result result = phase4_result(
		v34_phase4_line_receiver_sample(
			&receiver->state.phase4, sample, &emitted));

	if (terminal_result(result)) {
		receiver->state.phase4.failed = true;
		receiver->failed = true;
	}
	if (result == V34_TRAINING_RECEIVER_RECORD_READY)
		*event = emitted;
	return result;
}

enum v34_training_receiver_result v34_training_receiver_sample(
	struct v34_training_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event)
{
	if (!v34_training_receiver_is_valid(receiver) || event == NULL)
		return V34_TRAINING_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_TRAINING_RECEIVER_FAILED;
	if (receiver->stage == V34_TRAINING_RECEIVER_PHASE4 &&
	    receiver->state.phase4.control.stage ==
		    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_TRAINING_RECEIVER_COMPLETE;
	return receiver->stage == V34_TRAINING_RECEIVER_PHASE3 ?
		       sample_phase3(receiver, sample) :
		       sample_phase4(receiver, sample, event);
}

static enum v34_training_receiver_result update_phase4(
	struct v34_training_receiver *receiver,
	enum v34_phase4_line_receiver_result (*update)(
		struct v34_phase4_line_receiver *))
{
	struct v34_training_receiver next;
	enum v34_training_receiver_result result;

	if (!v34_training_receiver_is_valid(receiver))
		return V34_TRAINING_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_TRAINING_RECEIVER_FAILED;
	if (receiver->stage != V34_TRAINING_RECEIVER_PHASE4)
		return V34_TRAINING_RECEIVER_INVALID_STATE;
	next = *receiver;
	result = phase4_result(update(&next.state.phase4));
	if (result != V34_TRAINING_RECEIVER_OK)
		return result;
	if (!v34_training_receiver_is_valid(&next))
		return V34_TRAINING_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return V34_TRAINING_RECEIVER_OK;
}

enum v34_training_receiver_result v34_training_receiver_continue_mp(
	struct v34_training_receiver *receiver)
{
	return update_phase4(
		receiver, v34_phase4_line_receiver_continue_mp);
}

enum v34_training_receiver_result v34_training_receiver_start_e(
	struct v34_training_receiver *receiver)
{
	return update_phase4(receiver, v34_phase4_line_receiver_start_e);
}

enum v34_training_receiver_result v34_training_receiver_start_data(
	const struct v34_training_receiver *receiver,
	struct v34_data_line_receiver *data_receiver,
	const struct v34_data_line_receiver_configuration *configuration)
{
	enum v34_phase4_line_receiver_result result;

	if (!v34_training_receiver_is_valid(receiver) ||
	    data_receiver == NULL || configuration == NULL)
		return V34_TRAINING_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_TRAINING_RECEIVER_FAILED;
	if (receiver->stage != V34_TRAINING_RECEIVER_PHASE4)
		return V34_TRAINING_RECEIVER_INVALID_STATE;
	result = v34_phase4_line_receiver_start_data(
		&receiver->state.phase4, data_receiver, configuration);
	return result == V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT ?
		       V34_TRAINING_RECEIVER_INVALID_ARGUMENT :
		       phase4_result(result);
}

const char *v34_training_receiver_result_name(
	enum v34_training_receiver_result result)
{
	switch (result) {
	case V34_TRAINING_RECEIVER_OK:
		return "ok";
	case V34_TRAINING_RECEIVER_TIMING_ACQUIRED:
		return "timing acquired";
	case V34_TRAINING_RECEIVER_S_DETECTED:
		return "S detected";
	case V34_TRAINING_RECEIVER_MD_COMPLETE:
		return "MD complete";
	case V34_TRAINING_RECEIVER_REPEATED_S_DETECTED:
		return "repeated S detected";
	case V34_TRAINING_RECEIVER_PP_COMPLETE:
		return "PP complete";
	case V34_TRAINING_RECEIVER_J_DETECTED:
		return "J detected";
	case V34_TRAINING_RECEIVER_SYMBOL:
		return "symbol";
	case V34_TRAINING_RECEIVER_RECORD_READY:
		return "record ready";
	case V34_TRAINING_RECEIVER_E_COMPLETE:
		return "E complete";
	case V34_TRAINING_RECEIVER_COMPLETE:
		return "complete";
	case V34_TRAINING_RECEIVER_FAILED:
		return "failed";
	case V34_TRAINING_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_TRAINING_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V34_TRAINING_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_TRAINING_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	case V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	}
	return "unknown";
}
