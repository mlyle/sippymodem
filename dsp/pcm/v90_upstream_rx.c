// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_rx.h"

#include "v34_data_mapping.h"
#include "v90_upstream_mode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct v90_mp_parameters *mp_common(
	const struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static bool initial_mp_is_valid(const struct v90_mp_record *record)
{
	return v90_mp_record_is_valid(record) &&
	       !mp_common(record)->acknowledges_cp;
}

static bool mappings_are_equal(
	const struct v90_pcm_mapping *left,
	const struct v90_pcm_mapping *right)
{
	return left->kind == right->kind && left->law == right->law &&
	       left->frame_bits == right->frame_bits &&
	       left->modulus_bits == right->modulus_bits &&
	       left->sign_bits == right->sign_bits &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead == right->spectral_lookahead &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->codec_plane_differs == right->codec_plane_differs &&
	       memcmp(left->transmit, right->transmit,
		      sizeof(left->transmit)) == 0 &&
	       memcmp(left->codec, right->codec, sizeof(left->codec)) == 0 &&
	       memcmp(left->modulus, right->modulus,
		      sizeof(left->modulus)) == 0;
}

static bool parameters_are_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	uint8_t left_frame[V90_CP_MAX_FRAME_BYTES] = { 0 };
	uint8_t right_frame[V90_CP_MAX_FRAME_BYTES] = { 0 };
	size_t left_bits = 0;
	size_t right_bits = 0;

	return v90_cp_encode(
		       left, left_frame, sizeof(left_frame), &left_bits) ==
		       V90_CP_OK &&
	       v90_cp_encode(
		       right, right_frame, sizeof(right_frame), &right_bits) ==
		       V90_CP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame,
		      (left_bits + 7u) / 8u) == 0;
}

static bool data_parameters_are_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	struct v90_cp_parameters normalized_left = *left;
	struct v90_cp_parameters normalized_right = *right;

	normalized_left.acknowledges_peer = false;
	normalized_right.acknowledges_peer = false;
	return parameters_are_equal(&normalized_left, &normalized_right);
}

static enum v90_upstream_rx_result validate_data_parameters(
	const struct v90_upstream_rx *receiver,
	const struct v90_cp_parameters *parameters)
{
	struct v90_cp_parameters final = *parameters;
	struct v90_pcm_mapping mapping;
	struct v34_data_direction_parameters direction;
	enum v90_upstream_mode_result result;

	if (!v90_cp_parameters_are_valid(parameters) ||
	    parameters->kind != V90_CP_DATA ||
	    parameters->selected_rate_index == 0 ||
	    parameters->request_silence ||
	    parameters->law != receiver->training_mapping.law)
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	if (v90_pcm_mapping_init(&mapping, parameters) != V90_PCM_MAPPING_OK)
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	final.acknowledges_peer = true;
	result = v90_upstream_mode_select(
		&final, &receiver->local_mp,
		&receiver->state.cp.transmitting_profile, &direction);
	switch (result) {
	case V90_UPSTREAM_MODE_OK:
		return V90_UPSTREAM_RX_OK;
	case V90_UPSTREAM_MODE_NO_COMMON_RATE:
		return V90_UPSTREAM_RX_NO_COMMON_RATE;
	case V90_UPSTREAM_MODE_INVALID_PARAMETERS:
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	case V90_UPSTREAM_MODE_INVALID_ARGUMENT:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

bool v90_upstream_rx_is_valid(const struct v90_upstream_rx *receiver)
{
	if (receiver == NULL ||
	    !v90_pcm_mapping_is_valid(&receiver->training_mapping) ||
	    receiver->training_mapping.kind != V90_CP_TRAINING ||
	    !initial_mp_is_valid(&receiver->local_mp) ||
	    (receiver->stage != V90_UPSTREAM_RX_STAGE_CP &&
	     !receiver->data_cp_received) ||
	    (receiver->data_cp_received &&
	     (!v90_cp_parameters_are_valid(&receiver->data_parameters) ||
	      receiver->data_parameters.kind != V90_CP_DATA ||
	      receiver->data_parameters.acknowledges_peer ||
	      receiver->data_parameters.request_silence ||
	      receiver->data_parameters.selected_rate_index == 0 ||
	      receiver->data_parameters.law !=
		      receiver->training_mapping.law)))
		return false;

	switch (receiver->stage) {
	case V90_UPSTREAM_RX_STAGE_CP:
		return v90_phase4_cp_line_rx_is_valid(&receiver->state.cp) &&
		       !v90_phase4_cp_line_rx_has_pending_record(
			       &receiver->state.cp);
	case V90_UPSTREAM_RX_STAGE_E:
		return v90_phase4_e_line_rx_is_valid(&receiver->state.e) &&
		       data_parameters_are_equal(
			       &receiver->data_parameters,
			       &receiver->state.e.cp) &&
		       receiver->state.e.cp.acknowledges_peer &&
		       (receiver->failed ||
			!v90_phase4_e_line_rx_is_complete(
				&receiver->state.e));
	case V90_UPSTREAM_RX_STAGE_B1:
		return v34_data_line_receiver_is_valid(&receiver->state.data) &&
		       (receiver->failed ||
			v34_data_line_receiver_is_receiving_b1(
				&receiver->state.data));
	case V90_UPSTREAM_RX_STAGE_COMPLETE:
		return !receiver->failed &&
		       v34_data_line_receiver_is_valid(&receiver->state.data) &&
		       !v34_data_line_receiver_has_failed(
			       &receiver->state.data) &&
		       !v34_data_line_receiver_is_receiving_b1(
			       &receiver->state.data);
	}
	return false;
}

bool v90_upstream_rx_has_failed(const struct v90_upstream_rx *receiver)
{
	return v90_upstream_rx_is_valid(receiver) && receiver->failed;
}

bool v90_upstream_rx_is_complete(const struct v90_upstream_rx *receiver)
{
	return v90_upstream_rx_is_valid(receiver) && !receiver->failed &&
	       receiver->stage == V90_UPSTREAM_RX_STAGE_COMPLETE;
}

enum v90_upstream_rx_result v90_upstream_rx_init(
	struct v90_upstream_rx *receiver,
	const struct v90_phase4_cp_line_rx *cp,
	const struct v90_pcm_mapping *training_mapping,
	const struct v90_mp_record *local_mp)
{
	struct v90_upstream_rx initialized = {
		.stage = V90_UPSTREAM_RX_STAGE_CP,
	};

	if (receiver == NULL ||
	    !v90_phase4_cp_line_rx_is_valid(cp) ||
	    v90_phase4_cp_line_rx_has_failed(cp) ||
	    v90_phase4_cp_line_rx_has_pending_record(cp) ||
	    !v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING ||
	    !initial_mp_is_valid(local_mp))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	initialized.state.cp = *cp;
	initialized.training_mapping = *training_mapping;
	initialized.local_mp = *local_mp;
	if (!v90_upstream_rx_is_valid(&initialized))
		return V90_UPSTREAM_RX_INVALID_PARAMETERS;
	*receiver = initialized;
	return V90_UPSTREAM_RX_OK;
}

static enum v90_upstream_rx_result fail_receiver(
	struct v90_upstream_rx *receiver,
	struct v90_upstream_rx *next,
	enum v90_upstream_rx_result result)
{
	next->failed = true;
	if (v90_upstream_rx_is_valid(next))
		*receiver = *next;
	else
		receiver->failed = true;
	return result;
}

static enum v90_upstream_rx_result continue_cp(
	struct v90_upstream_rx *receiver)
{
	return v90_phase4_cp_line_rx_continue(&receiver->state.cp) ==
		       V90_PHASE4_CP_LINE_RX_OK ?
		       V90_UPSTREAM_RX_OK :
		       V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

static enum v90_upstream_rx_result accept_cp_record(
	struct v90_upstream_rx *receiver,
	const struct v90_cp_receive_event *event,
	struct v90_cp_parameters *emitted,
	bool *event_ready)
{
	struct v90_cp_parameters parameters;
	struct v90_pcm_mapping mapping;
	enum v90_upstream_rx_result result;

	if (!v90_phase4_cp_line_rx_pending_record_is_valid(
		    &receiver->state.cp))
		return continue_cp(receiver);
	if (v90_phase4_cp_line_rx_pending_parameters(
		    &receiver->state.cp, &parameters) !=
		    V90_PHASE4_CP_LINE_RX_OK ||
	    !parameters_are_equal(&parameters, &event->parameters))
		return V90_UPSTREAM_RX_NUMERIC_FAILURE;
	if (parameters.kind == V90_CP_TRAINING) {
		if (receiver->data_cp_received ||
		    v90_pcm_mapping_init(&mapping, &parameters) !=
			    V90_PCM_MAPPING_OK ||
		    !mappings_are_equal(
			    &mapping, &receiver->training_mapping))
			return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
		return continue_cp(receiver);
	}
	result = validate_data_parameters(receiver, &parameters);
	if (result != V90_UPSTREAM_RX_OK)
		return result;
	if (!receiver->data_cp_received) {
		if (parameters.acknowledges_peer)
			return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
		receiver->data_cp_received = true;
		receiver->data_parameters = parameters;
		result = continue_cp(receiver);
		if (result != V90_UPSTREAM_RX_OK)
			return result;
		*emitted = parameters;
		*event_ready = true;
		return V90_UPSTREAM_RX_REQUEST_MP_PRIME;
	}
	if (!data_parameters_are_equal(
		    &parameters, &receiver->data_parameters))
		return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
	if (!parameters.acknowledges_peer)
		return continue_cp(receiver);
	{
		struct v90_phase4_e_line_rx e;

		if (v90_phase4_e_line_rx_init(
			    &e, &receiver->state.cp) !=
		    V90_PHASE4_E_LINE_RX_OK)
			return V90_UPSTREAM_RX_NUMERIC_FAILURE;
		receiver->state = (union v90_upstream_rx_state){ 0 };
		receiver->state.e = e;
		receiver->stage = V90_UPSTREAM_RX_STAGE_E;
	}
	*emitted = parameters;
	*event_ready = true;
	return V90_UPSTREAM_RX_REQUEST_ED;
}

static enum v90_upstream_rx_result receive_cp_sample(
	struct v90_upstream_rx *receiver, int16_t sample,
	struct v90_cp_parameters *emitted, bool *event_ready)
{
	struct v90_cp_receive_event event = { 0 };
	enum v90_phase4_cp_line_rx_result result =
		v90_phase4_cp_line_rx_sample(
			&receiver->state.cp, sample, &event);

	switch (result) {
	case V90_PHASE4_CP_LINE_RX_OK:
	case V90_PHASE4_CP_LINE_RX_SYMBOL:
		return V90_UPSTREAM_RX_OK;
	case V90_PHASE4_CP_LINE_RX_RECORD_READY:
		return accept_cp_record(
			receiver, &event, emitted, event_ready);
	case V90_PHASE4_CP_LINE_RX_FAILED:
		return V90_UPSTREAM_RX_FAILED;
	case V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE4_CP_LINE_RX_INVALID_STATE:
	case V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

static enum v90_upstream_rx_result receive_e_sample(
	struct v90_upstream_rx *receiver, int16_t sample)
{
	struct v90_phase4_e_line_rx e = receiver->state.e;
	enum v90_phase4_e_line_rx_result result =
		v90_phase4_e_line_rx_sample(&e, sample);

	switch (result) {
	case V90_PHASE4_E_LINE_RX_OK:
	case V90_PHASE4_E_LINE_RX_SYMBOL:
		receiver->state.e = e;
		return V90_UPSTREAM_RX_OK;
	case V90_PHASE4_E_LINE_RX_E_COMPLETE: {
		struct v34_data_line_receiver data;

		if (v90_upstream_rx_start_after_e(
			    &data, &e, &receiver->local_mp) !=
		    V90_UPSTREAM_DATA_OK)
			return V90_UPSTREAM_RX_NUMERIC_FAILURE;
		receiver->state = (union v90_upstream_rx_state){ 0 };
		receiver->state.data = data;
		receiver->stage = V90_UPSTREAM_RX_STAGE_B1;
		return V90_UPSTREAM_RX_E_COMPLETE;
	}
	case V90_PHASE4_E_LINE_RX_FAILED:
		return V90_UPSTREAM_RX_FAILED;
	case V90_PHASE4_E_LINE_RX_SEQUENCE_MISMATCH:
		return V90_UPSTREAM_RX_SEQUENCE_MISMATCH;
	case V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT:
	case V90_PHASE4_E_LINE_RX_INVALID_STATE:
	case V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE:
	case V90_PHASE4_E_LINE_RX_COMPLETE:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

static enum v90_upstream_rx_result receive_b1_sample(
	struct v90_upstream_rx *receiver, int16_t sample)
{
	struct v34_survivor_decision current = { 0 };
	struct v34_received_mapping_frame received = { 0 };
	enum v34_data_line_receiver_result result =
		v34_data_line_receiver_sample(
			&receiver->state.data, sample, &current, &received);

	switch (result) {
	case V34_DATA_LINE_RECEIVER_OK:
	case V34_DATA_LINE_RECEIVER_INTERVAL_READY:
	case V34_DATA_LINE_RECEIVER_B1_IN_PROGRESS:
		return V90_UPSTREAM_RX_OK;
	case V34_DATA_LINE_RECEIVER_B1_COMPLETE:
		receiver->stage = V90_UPSTREAM_RX_STAGE_COMPLETE;
		return V90_UPSTREAM_RX_B1_COMPLETE;
	case V34_DATA_LINE_RECEIVER_FAILED:
		return V90_UPSTREAM_RX_FAILED;
	case V34_DATA_LINE_RECEIVER_OUT_OF_RANGE:
		return V90_UPSTREAM_RX_OUT_OF_RANGE;
	case V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT:
	case V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE:
	case V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY:
		break;
	}
	return V90_UPSTREAM_RX_NUMERIC_FAILURE;
}

static bool ranges_overlap(
	const void *left, size_t left_size,
	const void *right, size_t right_size)
{
	uintptr_t left_address = (uintptr_t)left;
	uintptr_t right_address = (uintptr_t)right;

	return left_address <= right_address ?
		       right_address - left_address < left_size :
		       left_address - right_address < right_size;
}

enum v90_upstream_rx_result v90_upstream_rx_sample(
	struct v90_upstream_rx *receiver, int16_t sample,
	struct v90_cp_parameters *event)
{
	struct v90_upstream_rx next;
	struct v90_cp_parameters emitted = { 0 };
	bool event_ready = false;
	enum v90_upstream_rx_result result;

	if (!v90_upstream_rx_is_valid(receiver) || event == NULL ||
	    ranges_overlap(
		    receiver, sizeof(*receiver), event, sizeof(*event)))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_RX_FAILED;
	if (receiver->stage == V90_UPSTREAM_RX_STAGE_COMPLETE)
		return V90_UPSTREAM_RX_COMPLETE;
	next = *receiver;
	switch (next.stage) {
	case V90_UPSTREAM_RX_STAGE_CP:
		result = receive_cp_sample(
			&next, sample, &emitted, &event_ready);
		break;
	case V90_UPSTREAM_RX_STAGE_E:
		result = receive_e_sample(&next, sample);
		break;
	case V90_UPSTREAM_RX_STAGE_B1:
		result = receive_b1_sample(&next, sample);
		break;
	case V90_UPSTREAM_RX_STAGE_COMPLETE:
		return V90_UPSTREAM_RX_COMPLETE;
	default:
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	}
	if (result == V90_UPSTREAM_RX_FAILED ||
	    result == V90_UPSTREAM_RX_SEQUENCE_MISMATCH ||
	    result == V90_UPSTREAM_RX_INVALID_PARAMETERS ||
	    result == V90_UPSTREAM_RX_NO_COMMON_RATE ||
	    result == V90_UPSTREAM_RX_OUT_OF_RANGE ||
	    result == V90_UPSTREAM_RX_NUMERIC_FAILURE)
		return fail_receiver(receiver, &next, result);
	if (!v90_upstream_rx_is_valid(&next))
		return fail_receiver(
			receiver, &next, V90_UPSTREAM_RX_NUMERIC_FAILURE);
	*receiver = next;
	if (event_ready)
		*event = emitted;
	return result;
}

enum v90_upstream_rx_result v90_upstream_rx_handoff(
	const struct v90_upstream_rx *receiver,
	struct v34_data_line_receiver *data_receiver)
{
	if (!v90_upstream_rx_is_valid(receiver) || data_receiver == NULL ||
	    ranges_overlap(
		    receiver, sizeof(*receiver), data_receiver,
		    sizeof(*data_receiver)))
		return V90_UPSTREAM_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V90_UPSTREAM_RX_FAILED;
	if (receiver->stage != V90_UPSTREAM_RX_STAGE_COMPLETE)
		return V90_UPSTREAM_RX_INVALID_STATE;
	*data_receiver = receiver->state.data;
	return V90_UPSTREAM_RX_OK;
}

static enum v90_upstream_data_result mapping_result(
	enum v34_data_mapping_result result)
{
	switch (result) {
	case V34_DATA_MAPPING_OK:
		return V90_UPSTREAM_DATA_OK;
	case V34_DATA_MAPPING_OUT_OF_RANGE:
		return V90_UPSTREAM_DATA_OUT_OF_RANGE;
	case V34_DATA_MAPPING_INVALID_ARGUMENT:
		return V90_UPSTREAM_DATA_INVALID_PARAMETERS;
	case V34_DATA_MAPPING_NUMERIC_FAILURE:
		break;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

static enum v90_upstream_data_result mode_result(
	enum v90_upstream_mode_result result)
{
	switch (result) {
	case V90_UPSTREAM_MODE_OK:
		return V90_UPSTREAM_DATA_OK;
	case V90_UPSTREAM_MODE_INVALID_ARGUMENT:
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	case V90_UPSTREAM_MODE_INVALID_PARAMETERS:
		return V90_UPSTREAM_DATA_INVALID_PARAMETERS;
	case V90_UPSTREAM_MODE_NO_COMMON_RATE:
		return V90_UPSTREAM_DATA_NO_COMMON_RATE;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

enum v90_upstream_data_result v90_upstream_rx_start_after_e(
	struct v34_data_line_receiver *receiver,
	const struct v90_phase4_e_line_rx *completed_e,
	const struct v90_mp_record *mp)
{
	struct v34_data_direction_parameters direction;
	struct v34_data_mapping mapping;
	struct v34_data_line_receiver_configuration configuration = { 0 };
	enum v90_upstream_data_result result;

	if (receiver == NULL || completed_e == NULL || mp == NULL)
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	if (!v90_phase4_e_line_rx_is_complete(completed_e))
		return V90_UPSTREAM_DATA_INVALID_STATE;
	result = mode_result(v90_upstream_mode_select(
		&completed_e->cp, mp, &completed_e->transmitting_profile,
		&direction));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	result = mapping_result(v34_data_mapping_prepare(
		&mapping, &completed_e->transmitting_profile, &direction,
		false));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	configuration.transmitting_profile =
		completed_e->transmitting_profile;
	configuration.geometry = mapping.schedule.geometry;
	configuration.normalizer = mapping.normalizer;
	configuration.equalizer_adaptation = completed_e->adaptation;
	configuration.trellis_selection = direction.trellis_selection;
	configuration.expanded_shaping = direction.expanded_shaping;
	memcpy(configuration.precoder_coefficients,
	       direction.precoder_coefficients,
	       sizeof(configuration.precoder_coefficients));
	return v90_phase4_e_line_rx_start_data(
		       completed_e, receiver, &configuration) ==
		       V90_PHASE4_E_LINE_RX_OK ?
		       V90_UPSTREAM_DATA_OK :
		       V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

const char *v90_upstream_rx_result_name(
	enum v90_upstream_rx_result result)
{
	switch (result) {
	case V90_UPSTREAM_RX_OK:
		return "ok";
	case V90_UPSTREAM_RX_REQUEST_MP_PRIME:
		return "request MP prime";
	case V90_UPSTREAM_RX_REQUEST_ED:
		return "request Ed";
	case V90_UPSTREAM_RX_E_COMPLETE:
		return "E complete";
	case V90_UPSTREAM_RX_B1_COMPLETE:
		return "B1 complete";
	case V90_UPSTREAM_RX_COMPLETE:
		return "complete";
	case V90_UPSTREAM_RX_FAILED:
		return "failed";
	case V90_UPSTREAM_RX_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_UPSTREAM_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_UPSTREAM_RX_INVALID_STATE:
		return "invalid state";
	case V90_UPSTREAM_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_UPSTREAM_RX_NO_COMMON_RATE:
		return "no common rate";
	case V90_UPSTREAM_RX_OUT_OF_RANGE:
		return "out of range";
	case V90_UPSTREAM_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
