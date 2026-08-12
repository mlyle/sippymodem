// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_line_receiver.h"

#include <stddef.h>

static enum v34_station_role transmitting_role(
	const struct v34_phase4_equalizer_receiver *control)
{
	return control->mp_receiver.transmitting_role;
}

bool v34_phase4_line_receiver_is_valid(
	const struct v34_phase4_line_receiver *receiver)
{
	return receiver != NULL &&
	       v34_phase3_profile_is_valid(
		       &receiver->transmitting_profile) &&
	       qam_demodulator_is_valid(&receiver->demodulator) &&
	       qam_demodulator_measure(&receiver->demodulator).timing_locked &&
	       v34_data_carrier_channel_matches(
		       &receiver->transmitting_profile,
		       &receiver->demodulator.channel) &&
	       v34_phase4_equalizer_receiver_is_valid(&receiver->control) &&
	       receiver->transmitting_profile.local_role ==
		       transmitting_role(&receiver->control);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_init(
	struct v34_phase4_line_receiver *receiver,
	const struct qam_demodulator *demodulator,
	const struct qam_equalizer *equalizer,
	const struct v34_phase3_profile *transmitting_profile,
	enum v34_constellation_size constellation, double adaptation)
{
	struct v34_phase4_line_receiver initialized = { 0 };

	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    !qam_demodulator_is_valid(demodulator) ||
	    !qam_demodulator_measure(demodulator).timing_locked ||
	    !v34_data_carrier_channel_matches(
		    transmitting_profile, &demodulator->channel) ||
	    v34_phase4_equalizer_receiver_init(
		    &initialized.control, equalizer,
		    transmitting_profile->local_role, constellation,
		    adaptation) != V34_PHASE4_EQUALIZER_RECEIVER_OK)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	initialized.demodulator = *demodulator;
	initialized.transmitting_profile = *transmitting_profile;
	if (!v34_phase4_line_receiver_is_valid(&initialized))
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_LINE_RECEIVER_OK;
}

bool v34_phase4_line_receiver_has_failed(
	const struct v34_phase4_line_receiver *receiver)
{
	return v34_phase4_line_receiver_is_valid(receiver) &&
	       receiver->failed;
}

static enum v34_phase4_line_receiver_result control_result(
	enum v34_phase4_equalizer_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_EQUALIZER_RECEIVER_OK:
	case V34_PHASE4_EQUALIZER_RECEIVER_WARMING:
		return V34_PHASE4_LINE_RECEIVER_OK;
	case V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL:
		return V34_PHASE4_LINE_RECEIVER_SYMBOL;
	case V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED:
		return V34_PHASE4_LINE_RECEIVER_S_DETECTED;
	case V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY:
		return V34_PHASE4_LINE_RECEIVER_RECORD_READY;
	case V34_PHASE4_EQUALIZER_RECEIVER_E_COMPLETE:
		return V34_PHASE4_LINE_RECEIVER_E_COMPLETE;
	case V34_PHASE4_EQUALIZER_RECEIVER_COMPLETE:
		return V34_PHASE4_LINE_RECEIVER_COMPLETE;
	case V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE:
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	case V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	case V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH:
		return V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH;
	case V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
}

static bool is_terminal(enum v34_phase4_line_receiver_result result)
{
	return result == V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE ||
	       result == V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH;
}

static enum v34_phase4_line_receiver_result update_control(
	struct v34_phase4_line_receiver *receiver,
	enum v34_phase4_equalizer_receiver_result (*update)(
		struct v34_phase4_equalizer_receiver *))
{
	struct v34_phase4_line_receiver next;
	enum v34_phase4_line_receiver_result result;

	if (!v34_phase4_line_receiver_is_valid(receiver))
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE4_LINE_RECEIVER_FAILED;
	next = *receiver;
	result = control_result(update(&next.control));
	if (result != V34_PHASE4_LINE_RECEIVER_OK)
		return result;
	if (!v34_phase4_line_receiver_is_valid(&next))
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE4_LINE_RECEIVER_OK;
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_continue_mp(
	struct v34_phase4_line_receiver *receiver)
{
	return update_control(
		receiver, v34_phase4_equalizer_receiver_continue_mp);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_start_e(
	struct v34_phase4_line_receiver *receiver)
{
	return update_control(
		receiver, v34_phase4_equalizer_receiver_start_e);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_expect_e(
	struct v34_phase4_line_receiver *receiver)
{
	return update_control(
		receiver, v34_phase4_equalizer_receiver_expect_e);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_sample(
	struct v34_phase4_line_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_line_receiver next;
	struct v34_mp_receive_event emitted = { 0 };
	struct qam_point point;
	enum v34_phase4_line_receiver_result result;
	bool ready;

	if (!v34_phase4_line_receiver_is_valid(receiver) || event == NULL)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE4_LINE_RECEIVER_FAILED;
	if (receiver->control.record_pending)
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	if (receiver->control.stage ==
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE4_LINE_RECEIVER_COMPLETE;
	next = *receiver;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &point, &ready) != QAM_OK) {
		receiver->failed = true;
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	if (!ready) {
		if (!v34_phase4_line_receiver_is_valid(&next)) {
			receiver->failed = true;
			return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
		}
		*receiver = next;
		return V34_PHASE4_LINE_RECEIVER_OK;
	}
	result = control_result(v34_phase4_equalizer_receive_point(
		&next.control, &point, &emitted));
	if (is_terminal(result)) {
		next.failed = true;
		if (!v34_phase4_line_receiver_is_valid(&next))
			receiver->failed = true;
		else
			*receiver = next;
		return result;
	}
	if (result == V34_PHASE4_LINE_RECEIVER_INVALID_STATE ||
	    !v34_phase4_line_receiver_is_valid(&next)) {
		receiver->failed = true;
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	*receiver = next;
	if (result == V34_PHASE4_LINE_RECEIVER_RECORD_READY)
		*event = emitted;
	return result;
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_start_data(
	const struct v34_phase4_line_receiver *receiver,
	struct v34_data_line_receiver *data_receiver,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_equalizer_handoff handoff;

	if (!v34_phase4_line_receiver_is_valid(receiver) ||
	    data_receiver == NULL || configuration == NULL)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE4_LINE_RECEIVER_FAILED;
	if (receiver->control.stage !=
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	if (v34_phase4_equalizer_receiver_handoff(
		    &receiver->control, &handoff) !=
		    V34_PHASE4_EQUALIZER_RECEIVER_OK)
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	if (v34_data_line_receiver_init(
		    data_receiver, &receiver->demodulator, &handoff,
		    configuration) != V34_DATA_LINE_RECEIVER_OK)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	return V34_PHASE4_LINE_RECEIVER_OK;
}

const char *v34_phase4_line_receiver_result_name(
	enum v34_phase4_line_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_LINE_RECEIVER_OK:
		return "ok";
	case V34_PHASE4_LINE_RECEIVER_SYMBOL:
		return "symbol";
	case V34_PHASE4_LINE_RECEIVER_S_DETECTED:
		return "S detected";
	case V34_PHASE4_LINE_RECEIVER_RECORD_READY:
		return "record ready";
	case V34_PHASE4_LINE_RECEIVER_E_COMPLETE:
		return "E complete";
	case V34_PHASE4_LINE_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE4_LINE_RECEIVER_FAILED:
		return "failed";
	case V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE4_LINE_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	}
	return "unknown";
}
