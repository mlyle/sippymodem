// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_exchange.h"

#include <stddef.h>

static bool outputs_overlap(
	const int16_t *sample, const uint32_t *events)
{
	uintptr_t sample_address = (uintptr_t)(const void *)sample;
	uintptr_t event_address = (uintptr_t)(const void *)events;

	return sample_address <= event_address ?
		       event_address - sample_address < sizeof(*sample) :
		       sample_address - event_address < sizeof(*events);
}

static bool trn_minimum_complete(
	const struct v34_phase4_exchange *exchange)
{
	return v34_training_transmitter_phase4_trn_ready(&exchange->transmit);
}

static bool transmitter_stage_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	enum v34_training_transmitter_stage stage = exchange->transmit.stage;
	bool in_trn = stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN;

	if (exchange->transmit_trn_started != in_trn)
		return false;
	if (exchange->local_role == V34_CALLING_STATION)
		return stage == V34_TRAINING_TRANSMITTER_PHASE3 ||
		       stage == V34_TRAINING_TRANSMITTER_J_PRIME || in_trn;
	return exchange->local_role == V34_ANSWERING_STATION &&
	       (stage == V34_TRAINING_TRANSMITTER_ANSWER_S || in_trn);
}

bool v34_phase4_exchange_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	enum v34_station_role peer_role;

	if (exchange == NULL ||
	    !v34_training_transmitter_is_valid(&exchange->transmit) ||
	    !v34_training_receiver_is_valid(&exchange->receive) ||
	    exchange->receive.stage != V34_TRAINING_RECEIVER_PHASE4 ||
	    !transmitter_stage_is_valid(exchange))
		return false;
	peer_role = exchange->receive.state.phase4.transmitting_profile.local_role;
	if (exchange->local_role != exchange->transmit.phase3.local_role ||
	    exchange->local_role == peer_role ||
	    exchange->transmit.line.carrier.channel.sample_rate !=
		    exchange->receive.state.phase4.demodulator.channel.sample_rate ||
	    (v34_training_receiver_has_failed(&exchange->receive) &&
	     !exchange->failed) ||
	    (exchange->receive.state.phase4.control.record_pending &&
	     !exchange->failed) ||
	    exchange->transmit_trn_ready_announced !=
		    trn_minimum_complete(exchange))
		return false;
	if (exchange->local_role == V34_ANSWERING_STATION)
		return !exchange->finish_j_requested;
	if (exchange->local_role != V34_CALLING_STATION)
		return false;
	if (exchange->finish_j_requested)
		return exchange->transmit.stage ==
		       V34_TRAINING_TRANSMITTER_PHASE3;
	return exchange->transmit.stage !=
		       V34_TRAINING_TRANSMITTER_PHASE3 ||
	       exchange->receive.state.phase4.control.stage ==
		       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S;
}

static enum v34_phase4_exchange_result transmit_result(
	enum v34_training_transmitter_result result)
{
	switch (result) {
	case V34_TRAINING_TRANSMITTER_OK:
		return V34_PHASE4_EXCHANGE_OK;
	case V34_TRAINING_TRANSMITTER_OUT_OF_RANGE:
		return V34_PHASE4_EXCHANGE_OUT_OF_RANGE;
	case V34_TRAINING_TRANSMITTER_CLIPPING_RISK:
		return V34_PHASE4_EXCHANGE_CLIPPING_RISK;
	case V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT:
	case V34_TRAINING_TRANSMITTER_INVALID_STATE:
	case V34_TRAINING_TRANSMITTER_INVALID_PARAMETERS:
	case V34_TRAINING_TRANSMITTER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

enum v34_phase4_exchange_result v34_phase4_exchange_init(
	struct v34_phase4_exchange *exchange,
	const struct v34_phase3_exchange_handoff *handoff,
	const struct v34_phase4_exchange_configuration *configuration)
{
	struct v34_phase4_exchange initialized = { 0 };
	enum v34_training_transmitter_result transmit;
	enum v34_training_receiver_result receive;

	if (exchange == NULL ||
	    !v34_phase3_exchange_handoff_is_valid(handoff) ||
	    configuration == NULL ||
	    !v34_training_receiver_configuration_is_valid(
		    &configuration->receive))
		return V34_PHASE4_EXCHANGE_INVALID_ARGUMENT;
	transmit = v34_training_transmitter_continue(
		&initialized.transmit, &handoff->transmit_line,
		&handoff->phase3_transmit);
	if (transmit != V34_TRAINING_TRANSMITTER_OK)
		return transmit_result(transmit);
	receive = v34_training_receiver_continue(
		&initialized.receive, &handoff->receive,
		&configuration->receive);
	if (receive != V34_TRAINING_RECEIVER_OK)
		return receive == V34_TRAINING_RECEIVER_OUT_OF_RANGE ?
		       V34_PHASE4_EXCHANGE_OUT_OF_RANGE :
		       V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	initialized.local_role = handoff->transmit_line.profile.local_role;
	if (!v34_phase4_exchange_is_valid(&initialized))
		return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	*exchange = initialized;
	return V34_PHASE4_EXCHANGE_OK;
}

bool v34_phase4_exchange_has_failed(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && exchange->failed;
}

bool v34_phase4_exchange_is_trn_ready(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && !exchange->failed &&
	       trn_minimum_complete(exchange);
}

static enum v34_phase4_exchange_result prepare_transmitter(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	if (exchange->finish_j_requested &&
	    exchange->transmit.stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
	    exchange->transmit.phase3.j_symbols >=
		    V34_PHASE3_J_PATTERN_SYMBOLS &&
	    v34_phase3_j_sequence_at_pattern_boundary(
		    &exchange->transmit.phase3.state.j)) {
		if (v34_training_transmitter_finish_j(&exchange->transmit) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
		exchange->finish_j_requested = false;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_J_FINISHED;
	}
	return V34_PHASE4_EXCHANGE_OK;
}

static void note_transmitter(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	if (!exchange->transmit_trn_started &&
	    exchange->transmit.stage ==
		    V34_TRAINING_TRANSMITTER_PHASE4_TRN) {
		exchange->transmit_trn_started = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED;
	}
	if (!exchange->transmit_trn_ready_announced &&
	    trn_minimum_complete(exchange)) {
		exchange->transmit_trn_ready_announced = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_READY;
	}
}

static enum v34_phase4_exchange_result drive_receiver(
	struct v34_phase4_exchange *exchange, int16_t sample,
	uint32_t *events)
{
	struct v34_mp_receive_event event = { 0 };
	enum v34_training_receiver_result result =
		v34_training_receiver_sample(&exchange->receive, sample, &event);

	switch (result) {
	case V34_TRAINING_RECEIVER_OK:
	case V34_TRAINING_RECEIVER_SYMBOL:
		return V34_PHASE4_EXCHANGE_OK;
	case V34_TRAINING_RECEIVER_S_DETECTED:
		if (exchange->local_role != V34_CALLING_STATION ||
		    exchange->transmit.stage !=
			    V34_TRAINING_TRANSMITTER_PHASE3 ||
		    exchange->finish_j_requested)
			return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
		exchange->finish_j_requested = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED;
		return V34_PHASE4_EXCHANGE_OK;
	case V34_TRAINING_RECEIVER_FAILED:
		return V34_PHASE4_EXCHANGE_FAILED;
	case V34_TRAINING_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE4_EXCHANGE_OUT_OF_RANGE;
	case V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH:
	case V34_TRAINING_RECEIVER_RECORD_READY:
	case V34_TRAINING_RECEIVER_E_COMPLETE:
	case V34_TRAINING_RECEIVER_COMPLETE:
		return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
	case V34_TRAINING_RECEIVER_TIMING_ACQUIRED:
	case V34_TRAINING_RECEIVER_MD_COMPLETE:
	case V34_TRAINING_RECEIVER_REPEATED_S_DETECTED:
	case V34_TRAINING_RECEIVER_PP_COMPLETE:
	case V34_TRAINING_RECEIVER_J_DETECTED:
	case V34_TRAINING_RECEIVER_INVALID_ARGUMENT:
	case V34_TRAINING_RECEIVER_INVALID_STATE:
	case V34_TRAINING_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static bool terminal_result(enum v34_phase4_exchange_result result)
{
	return result == V34_PHASE4_EXCHANGE_FAILED ||
	       result == V34_PHASE4_EXCHANGE_OUT_OF_RANGE ||
	       result == V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH ||
	       result == V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

enum v34_phase4_exchange_result v34_phase4_exchange_sample(
	struct v34_phase4_exchange *exchange, int16_t received_sample,
	int16_t *transmit_sample, uint32_t *events)
{
	struct v34_phase4_exchange next;
	enum v34_phase4_exchange_result result;
	int16_t generated;
	uint32_t emitted = 0;

	if (!v34_phase4_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || events == NULL ||
	    outputs_overlap(transmit_sample, events))
		return V34_PHASE4_EXCHANGE_INVALID_ARGUMENT;
	if (exchange->failed)
		return V34_PHASE4_EXCHANGE_FAILED;
	next = *exchange;
	result = prepare_transmitter(&next, &emitted);
	if (result != V34_PHASE4_EXCHANGE_OK) {
		exchange->failed = true;
		return result;
	}
	result = transmit_result(v34_training_transmitter_sample(
		&next.transmit, &generated));
	if (result != V34_PHASE4_EXCHANGE_OK) {
		exchange->failed = true;
		return result;
	}
	note_transmitter(&next, &emitted);
	result = drive_receiver(&next, received_sample, &emitted);
	if (result == V34_PHASE4_EXCHANGE_OK)
		result = prepare_transmitter(&next, &emitted);
	if (terminal_result(result))
		next.failed = true;
	if (!v34_phase4_exchange_is_valid(&next)) {
		exchange->failed = true;
		return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	}
	*exchange = next;
	*transmit_sample = generated;
	*events = emitted;
	return result;
}

const char *v34_phase4_exchange_result_name(
	enum v34_phase4_exchange_result result)
{
	switch (result) {
	case V34_PHASE4_EXCHANGE_OK:
		return "ok";
	case V34_PHASE4_EXCHANGE_FAILED:
		return "failed";
	case V34_PHASE4_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE4_EXCHANGE_OUT_OF_RANGE:
		return "out of range";
	case V34_PHASE4_EXCHANGE_CLIPPING_RISK:
		return "clipping risk";
	case V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V34_PHASE4_EXCHANGE_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
