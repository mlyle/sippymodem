// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_exchange.h"

#include <stddef.h>
#include <string.h>

static struct v34_mp_parameters *record_common(
	struct v34_mp_record *record)
{
	return record->type == V34_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static const struct v34_mp_parameters *record_common_const(
	const struct v34_mp_record *record)
{
	return record->type == V34_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

bool v34_mp_record_is_valid(const struct v34_mp_record *record)
{
	if (record == NULL)
		return false;
	if (record->type == V34_MP_RECORD_TYPE0)
		return v34_mp_parameters_are_valid(&record->parameters.type0);
	return record->type == V34_MP_RECORD_TYPE1 &&
	       v34_mp_type1_parameters_are_valid(&record->parameters.type1);
}

static void set_record_acknowledgment(
	struct v34_mp_record *record, bool acknowledged)
{
	record_common(record)->acknowledges_peer_mp = acknowledged;
}

static bool record_is_acknowledged(const struct v34_mp_record *record)
{
	return record_common_const(record)->acknowledges_peer_mp;
}

static bool encode_record(
	const struct v34_mp_record *record,
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES], size_t *bit_count)
{
	enum v34_mp_result result;

	if (!v34_mp_record_is_valid(record) || frame == NULL ||
	    bit_count == NULL)
		return false;
	memset(frame, 0, V34_MP_TYPE1_FRAME_BYTES);
	if (record->type == V34_MP_RECORD_TYPE0) {
		result = v34_mp_type0_encode(
			&record->parameters.type0, frame,
			V34_MP_TYPE1_FRAME_BYTES);
		*bit_count = V34_MP_TYPE0_FRAME_BITS;
	} else {
		result = v34_mp_type1_encode(
			&record->parameters.type1, frame,
			V34_MP_TYPE1_FRAME_BYTES);
		*bit_count = V34_MP_TYPE1_FRAME_BITS;
	}
	return result == V34_MP_OK;
}

static bool records_match(
	const struct v34_mp_record *left,
	const struct v34_mp_record *right)
{
	uint8_t left_frame[V34_MP_TYPE1_FRAME_BYTES];
	uint8_t right_frame[V34_MP_TYPE1_FRAME_BYTES];
	size_t left_bits;
	size_t right_bits;

	return left->type == right->type &&
	       encode_record(left, left_frame, &left_bits) &&
	       encode_record(right, right_frame, &right_bits) &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

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
	return exchange->transmit.phase4_trn_symbols >=
	       V34_PHASE4_TRN_MIN_SYMBOLS;
}

static bool transmitter_mp_matches(
	const struct v34_phase4_exchange *exchange)
{
	struct v34_mp_record expected = exchange->transmit_mp;
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES];
	size_t bit_count;

	if (exchange->transmit.stage !=
	    V34_TRAINING_TRANSMITTER_PHASE4_MP)
		return true;
	set_record_acknowledgment(
		&expected, exchange->transmit_mp_prime_started);
	return encode_record(&expected, frame, &bit_count) &&
	       exchange->transmit.phase4.mp.frame_bit_count == bit_count &&
	       memcmp(exchange->transmit.phase4.mp.frame, frame,
		      sizeof(frame)) == 0;
}

static bool transmitter_stage_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	enum v34_training_transmitter_stage stage = exchange->transmit.stage;
	bool entered_trn = stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN ||
			   stage == V34_TRAINING_TRANSMITTER_PHASE4_MP;

	if (exchange->transmit_trn_started != entered_trn ||
	    exchange->transmit_mp_started !=
		    (stage == V34_TRAINING_TRANSMITTER_PHASE4_MP))
		return false;
	if (exchange->local_role == V34_CALLING_STATION)
		return stage == V34_TRAINING_TRANSMITTER_PHASE3 ||
		       stage == V34_TRAINING_TRANSMITTER_J_PRIME ||
		       entered_trn;
	return exchange->local_role == V34_ANSWERING_STATION &&
	       (stage == V34_TRAINING_TRANSMITTER_ANSWER_S || entered_trn);
}

bool v34_phase4_exchange_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	enum v34_station_role peer_role;

	if (exchange == NULL ||
	    !v34_training_transmitter_is_valid(&exchange->transmit) ||
	    !v34_training_receiver_is_valid(&exchange->receive) ||
	    !v34_mp_record_is_valid(&exchange->transmit_mp) ||
	    record_is_acknowledged(&exchange->transmit_mp) ||
	    exchange->receive.stage != V34_TRAINING_RECEIVER_PHASE4 ||
	    !transmitter_stage_is_valid(exchange) ||
	    !transmitter_mp_matches(exchange))
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
		    trn_minimum_complete(exchange) ||
	    (exchange->transmit_mp_started &&
	     !exchange->transmit_trn_ready_announced) ||
	    (exchange->transmit_mp_prime_started &&
	     (!exchange->transmit_mp_started || !exchange->peer_mp_received)) ||
	    (exchange->peer_mp_prime_received &&
	     !exchange->peer_mp_received) ||
	    exchange->ready_announced !=
		    (exchange->transmit_mp_prime_started &&
		     exchange->peer_mp_prime_received) ||
	    (exchange->peer_mp_received &&
	     (!v34_mp_record_is_valid(&exchange->peer_mp) ||
	      record_is_acknowledged(&exchange->peer_mp))))
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
		    &configuration->receive) ||
	    !v34_mp_record_is_valid(&configuration->transmit_mp) ||
	    record_is_acknowledged(&configuration->transmit_mp))
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
	initialized.transmit_mp = configuration->transmit_mp;
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

bool v34_phase4_exchange_is_ready_for_e(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && !exchange->failed &&
	       exchange->ready_announced;
}

bool v34_phase4_exchange_get_peer_mp(
	const struct v34_phase4_exchange *exchange,
	struct v34_mp_record *record)
{
	if (!v34_phase4_exchange_is_valid(exchange) || exchange->failed ||
	    !exchange->peer_mp_received || record == NULL)
		return false;
	*record = exchange->peer_mp;
	return true;
}

static enum v34_phase4_exchange_result start_transmit_mp(
	struct v34_phase4_exchange *exchange, bool acknowledged)
{
	struct v34_mp_record record = exchange->transmit_mp;
	enum v34_training_transmitter_result result;

	set_record_acknowledgment(&record, acknowledged);
	result = record.type == V34_MP_RECORD_TYPE0 ?
		v34_training_transmitter_start_mp_type0(
			&exchange->transmit, &record.parameters.type0) :
		v34_training_transmitter_start_mp_type1(
			&exchange->transmit, &record.parameters.type1);
	return transmit_result(result);
}

static enum v34_phase4_exchange_result prepare_transmitter(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	enum v34_phase4_exchange_result result;

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
	if (!exchange->transmit_mp_started && trn_minimum_complete(exchange)) {
		result = start_transmit_mp(exchange, false);
		if (result != V34_PHASE4_EXCHANGE_OK)
			return result;
		exchange->transmit_mp_started = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED;
	}
	if (exchange->transmit_mp_started &&
	    v34_training_transmitter_phase4_mp_record_complete(
		    &exchange->transmit)) {
		bool acknowledged = exchange->peer_mp_received;

		result = start_transmit_mp(exchange, acknowledged);
		if (result != V34_PHASE4_EXCHANGE_OK)
			return result;
		if (acknowledged && !exchange->transmit_mp_prime_started) {
			exchange->transmit_mp_prime_started = true;
			*events |=
				V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_PRIME_STARTED;
		}
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

static bool record_from_event(
	const struct v34_mp_receive_event *event,
	struct v34_mp_record *record)
{
	struct v34_mp_record decoded = { 0 };

	if (event->frame_result != V34_MP_OK)
		return false;
	if (event->type == V34_MP_RECEIVE_TYPE0) {
		decoded.type = V34_MP_RECORD_TYPE0;
		decoded.parameters.type0 = event->parameters.type0;
	} else if (event->type == V34_MP_RECEIVE_TYPE1) {
		decoded.type = V34_MP_RECORD_TYPE1;
		decoded.parameters.type1 = event->parameters.type1;
	} else {
		return false;
	}
	if (!v34_mp_record_is_valid(&decoded))
		return false;
	*record = decoded;
	return true;
}

static enum v34_phase4_exchange_result continue_mp_receiver(
	struct v34_phase4_exchange *exchange)
{
	enum v34_training_receiver_result result =
		v34_training_receiver_continue_mp(&exchange->receive);

	if (result == V34_TRAINING_RECEIVER_OK)
		return V34_PHASE4_EXCHANGE_OK;
	return result == V34_TRAINING_RECEIVER_FAILED ?
		       V34_PHASE4_EXCHANGE_FAILED :
		       V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result accept_mp_record(
	struct v34_phase4_exchange *exchange,
	const struct v34_mp_receive_event *event, uint32_t *events)
{
	struct v34_mp_record record;
	bool acknowledged;

	if (event->type == V34_MP_RECEIVE_REJECTED)
		return continue_mp_receiver(exchange);
	if (!record_from_event(event, &record))
		return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
	acknowledged = record_is_acknowledged(&record);
	set_record_acknowledgment(&record, false);
	if (!exchange->peer_mp_received) {
		exchange->peer_mp = record;
		exchange->peer_mp_received = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_MP_RECEIVED;
	} else if (!records_match(&exchange->peer_mp, &record)) {
		return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
	}
	if (acknowledged && !exchange->peer_mp_prime_received) {
		exchange->peer_mp_prime_received = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_MP_PRIME_RECEIVED;
	}
	return continue_mp_receiver(exchange);
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
	case V34_TRAINING_RECEIVER_RECORD_READY:
		return accept_mp_record(exchange, &event, events);
	case V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH:
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

static void note_ready(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	if (!exchange->ready_announced &&
	    exchange->transmit_mp_prime_started &&
	    exchange->peer_mp_prime_received) {
		exchange->ready_announced = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_READY_FOR_E;
	}
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
	if (result == V34_PHASE4_EXCHANGE_OK)
		note_ready(&next, &emitted);
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
