// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_info0_exchange.h"

#include <string.h>

static enum phase2_signal_role signal_for_role(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ? PHASE2_SIGNAL_A : PHASE2_SIGNAL_B;
}

static size_t payload_bits_for_role(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ?
		       V90_INFO0A_PAYLOAD_BITS : V90_INFO0D_PAYLOAD_BITS;
}

static enum v90_modem_role opposite_role(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ?
		       V90_DIGITAL_MODEM : V90_ANALOG_MODEM;
}

static struct v90_info0 *record_common(
	union pcm_info0_record *record, enum pcm_protocol protocol,
	enum v90_modem_role role)
{
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       &record->analog : &record->digital.common;
	return role == V90_ANALOG_MODEM ?
		       &record->v92_analog.v90 :
		       &record->v92_digital.v90.common;
}

static const struct v90_info0 *record_common_const(
	const union pcm_info0_record *record, enum pcm_protocol protocol,
	enum v90_modem_role role)
{
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       &record->analog : &record->digital.common;
	return role == V90_ANALOG_MODEM ?
		       &record->v92_analog.v90 :
		       &record->v92_digital.v90.common;
}

static bool record_is_valid(
	enum pcm_protocol protocol, enum v90_modem_role role,
	const union pcm_info0_record *record)
{
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       v90_info0_is_valid(&record->analog) :
			       v90_info0d_is_valid(&record->digital);
	return role == V90_ANALOG_MODEM ?
		       v92_info0a_is_valid(&record->v92_analog) :
		       v92_info0d_is_valid(&record->v92_digital);
}

static bool encode_record(
	enum pcm_protocol protocol, enum v90_modem_role role,
	const union pcm_info0_record *record, uint8_t *payload,
	size_t payload_capacity)
{
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       v90_info0a_encode(
				       &record->analog, payload,
				       payload_capacity) == V90_INFO0_OK :
			       v90_info0d_encode(
				       &record->digital, payload,
				       payload_capacity) == V90_INFO0_OK;
	return role == V90_ANALOG_MODEM ?
		       v92_info0a_encode(
			       &record->v92_analog, payload,
			       payload_capacity) == V92_INFO0_OK :
		       v92_info0d_encode(
			       &record->v92_digital, payload,
			       payload_capacity) == V92_INFO0_OK;
}

static bool decode_record(
	enum pcm_protocol protocol, enum v90_modem_role role,
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, union pcm_info0_record *record)
{
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       v90_info0a_decode(
				       payload, payload_capacity,
				       payload_bit_count,
				       &record->analog) == V90_INFO0_OK :
			       v90_info0d_decode(
				       payload, payload_capacity,
				       payload_bit_count,
				       &record->digital) == V90_INFO0_OK;
	return role == V90_ANALOG_MODEM ?
		       v92_info0a_decode(
			       payload, payload_capacity, payload_bit_count,
			       &record->v92_analog) == V92_INFO0_OK :
		       v92_info0d_decode(
			       payload, payload_capacity, payload_bit_count,
			       &record->v92_digital) == V92_INFO0_OK;
}

static bool decode_local_info0(
	const struct pcm_phase2_info0_exchange *exchange,
	union pcm_info0_record *local)
{
	const struct v34_phase2_info_generator *frame =
		&exchange->transport.info_transmitter.frame;

	return decode_record(
		exchange->protocol, exchange->local_role,
		frame->payload, sizeof(frame->payload),
		frame->payload_bit_count, local);
}

static bool local_info0_acknowledges_peer(
	const struct pcm_phase2_info0_exchange *exchange)
{
	union pcm_info0_record local = { 0 };

	return decode_local_info0(exchange, &local) &&
	       record_common_const(
		       &local, exchange->protocol,
		       exchange->local_role)->acknowledges_peer_info0;
}

static bool state_retains_peer_info0(
	enum pcm_phase2_info0_exchange_state state)
{
	return state == PCM_PHASE2_INFO0_VERIFY_REPEAT ||
	       state == PCM_PHASE2_INFO0_ACKNOWLEDGING ||
	       state == PCM_PHASE2_INFO0_ACK_REPEAT ||
	       state == PCM_PHASE2_INFO0_WAIT_COMPLETION ||
	       state == PCM_PHASE2_INFO0_COMPLETE ||
	       state == PCM_PHASE2_INFO0_RETRAIN;
}

static bool state_is_retrain(enum pcm_phase2_info0_exchange_state state)
{
	return state == PCM_PHASE2_INFO0_RETRAIN;
}

static bool peer_info0_is_valid(
	const struct pcm_phase2_info0_exchange *exchange)
{
	if (!state_retains_peer_info0(exchange->state))
		return true;
	return record_is_valid(
		exchange->protocol, opposite_role(exchange->local_role),
		&exchange->peer_info0);
}

static bool exchange_state_is_valid(
	enum pcm_phase2_info0_exchange_state state)
{
	return state >= PCM_PHASE2_INFO0_INITIAL &&
	       state <= PCM_PHASE2_INFO0_RETRAIN;
}

static bool exchange_state_contract_is_valid(
	const struct pcm_phase2_info0_exchange *exchange, bool local_ack)
{
	bool peer_ack = state_retains_peer_info0(exchange->state) &&
		record_common_const(
			&exchange->peer_info0, exchange->protocol,
			opposite_role(exchange->local_role))
			->acknowledges_peer_info0;

	switch (exchange->state) {
	case PCM_PHASE2_INFO0_INITIAL:
	case PCM_PHASE2_INFO0_REPEAT:
		return !local_ack && !exchange->fresh_peer_tone_seen;
	case PCM_PHASE2_INFO0_VERIFY_REPEAT:
		return local_ack && !exchange->fresh_peer_tone_seen &&
		       exchange->transport.info_receiver.frame.stage !=
			       V34_PHASE2_INFO_SEARCH_SYNC &&
		       exchange->transport.transmit_phase !=
			       PHASE2_INFO0_POST_V8_SILENCE;
	case PCM_PHASE2_INFO0_ACKNOWLEDGING:
		return (!exchange->fresh_peer_tone_seen || !peer_ack) &&
		       exchange->transport.transmit_phase != PHASE2_INFO0_TONE;
	case PCM_PHASE2_INFO0_ACK_REPEAT:
		return local_ack && !peer_ack &&
		       exchange->transport.transmit_phase !=
			       PHASE2_INFO0_TONE;
	case PCM_PHASE2_INFO0_WAIT_COMPLETION:
		return local_ack && !peer_ack &&
		       !exchange->fresh_peer_tone_seen &&
		       exchange->transport.transmit_phase == PHASE2_INFO0_TONE &&
		       !exchange->transport.tone_detector.carrier.tone_present;
	case PCM_PHASE2_INFO0_COMPLETE:
		return local_ack &&
		       (peer_ack != exchange->fresh_peer_tone_seen) &&
		       exchange->transport.transmit_phase == PHASE2_INFO0_TONE;
	case PCM_PHASE2_INFO0_RETRAIN:
		return !local_ack && !exchange->fresh_peer_tone_seen &&
		       exchange->transport.transmit_phase == PHASE2_INFO0_TONE;
	}
	return false;
}

bool pcm_phase2_info0_exchange_is_valid(
	const struct pcm_phase2_info0_exchange *exchange)
{
	union pcm_info0_record local = { 0 };
	bool local_ack;

	if (exchange == NULL ||
	    !pcm_modem_role_is_valid(exchange->local_role) ||
	    !pcm_protocol_is_valid(exchange->protocol) ||
	    !exchange_state_is_valid(exchange->state) ||
	    !phase2_info0_exchange_is_valid(&exchange->transport) ||
	    exchange->transport.signal_role !=
		    signal_for_role(exchange->local_role) ||
	    exchange->transport.info_transmitter.frame.payload_bit_count !=
		    payload_bits_for_role(exchange->local_role) ||
	    exchange->transport.info_receiver.frame.expected_payload_bit_count !=
		    payload_bits_for_role(opposite_role(exchange->local_role)) ||
	    !decode_local_info0(exchange, &local) ||
	    !peer_info0_is_valid(exchange))
		return false;
	local_ack = record_common_const(
		&local, exchange->protocol,
		exchange->local_role)->acknowledges_peer_info0;
	return exchange_state_contract_is_valid(exchange, local_ack);
}

bool pcm_phase2_info0_exchange_complete(
	const struct pcm_phase2_info0_exchange *exchange)
{
	return exchange != NULL &&
	       (exchange->state == PCM_PHASE2_INFO0_COMPLETE ||
		exchange->state == PCM_PHASE2_INFO0_RETRAIN);
}

static enum pcm_phase2_info0_exchange_result initialize(
	struct pcm_phase2_info0_exchange *exchange,
	enum pcm_protocol protocol, enum v90_modem_role role,
	int16_t nominal_amplitude,
	const union pcm_info0_record *local_info0)
{
	struct pcm_phase2_info0_exchange initialized = {
		.local_role = role,
		.protocol = protocol,
	};
	union pcm_info0_record initial;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };

	if (exchange == NULL || local_info0 == NULL ||
	    !pcm_protocol_is_valid(protocol) ||
	    !pcm_modem_role_is_valid(role) ||
	    !record_is_valid(protocol, role, local_info0))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0;
	record_common(
		&initial, protocol, role)->acknowledges_peer_info0 = false;
	if (!encode_record(
		    protocol, role, &initial, payload, sizeof(payload)) ||
	    phase2_info0_exchange_init(
		    &initialized.transport, signal_for_role(role),
		    nominal_amplitude, payload, payload_bits_for_role(role),
		    payload_bits_for_role(opposite_role(role))) !=
		    PHASE2_INFO0_EXCHANGE_OK ||
	    !pcm_phase2_info0_exchange_is_valid(&initialized))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return PCM_PHASE2_INFO0_EXCHANGE_OK;
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a)
{
	union pcm_info0_record local = { 0 };

	if (!v90_info0_is_valid(local_info0a))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	local.analog = *local_info0a;
	return initialize(
		exchange, PCM_PROTOCOL_V90, V90_ANALOG_MODEM,
		nominal_amplitude, &local);
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	union pcm_info0_record local = { 0 };

	if (!v90_info0d_is_valid(local_info0d))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	local.digital = *local_info0d;
	return initialize(
		exchange, PCM_PROTOCOL_V90, V90_DIGITAL_MODEM,
		nominal_amplitude, &local);
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a)
{
	union pcm_info0_record local = { 0 };

	if (!v92_info0a_is_valid(local_info0a) || !local_info0a->v92_capable)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	local.v92_analog = *local_info0a;
	return initialize(
		exchange, PCM_PROTOCOL_V92, V90_ANALOG_MODEM,
		nominal_amplitude, &local);
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d)
{
	union pcm_info0_record local = { 0 };

	if (!v92_info0d_is_valid(local_info0d) || !local_info0d->v92_capable)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	local.v92_digital = *local_info0d;
	return initialize(
		exchange, PCM_PROTOCOL_V92, V90_DIGITAL_MODEM,
		nominal_amplitude, &local);
}

static bool info0_semantics_match(
	const struct pcm_phase2_info0_exchange *exchange,
	const union pcm_info0_record *left,
	const union pcm_info0_record *right)
{
	union pcm_info0_record left_without_ack = *left;
	union pcm_info0_record right_without_ack = *right;
	uint8_t left_payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	uint8_t right_payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	enum v90_modem_role role = opposite_role(exchange->local_role);
	size_t payload_bytes = (payload_bits_for_role(role) + 7u) / 8u;

	record_common(
		&left_without_ack, exchange->protocol,
		role)->acknowledges_peer_info0 = false;
	record_common(
		&right_without_ack, exchange->protocol,
		role)->acknowledges_peer_info0 = false;
	return encode_record(
		       exchange->protocol, role, &left_without_ack,
		       left_payload, sizeof(left_payload)) &&
	       encode_record(
		       exchange->protocol, role, &right_without_ack,
		       right_payload, sizeof(right_payload)) &&
	       memcmp(left_payload, right_payload, payload_bytes) == 0;
}

static bool peer_acknowledges_local(
	const struct pcm_phase2_info0_exchange *exchange)
{
	return record_common_const(
		       &exchange->peer_info0, exchange->protocol,
		       opposite_role(exchange->local_role))
		->acknowledges_peer_info0;
}

static void clear_completion_tone(
	struct pcm_phase2_info0_exchange *exchange,
	struct pcm_phase2_info0_exchange_event *event)
{
	exchange->fresh_peer_tone_seen = false;
	event->tone_event = (struct v34_phase2_tone_event){ 0 };
}

static bool reject_peer_info0(
	struct pcm_phase2_info0_exchange *exchange,
	struct pcm_phase2_info0_exchange_event *event)
{
	event->peer_info0_event = PCM_PEER_INFO0_REJECTED;
	if (exchange->state == PCM_PHASE2_INFO0_VERIFY_REPEAT)
		exchange->state = PCM_PHASE2_INFO0_ACKNOWLEDGING;
	if (exchange->state != PCM_PHASE2_INFO0_COMPLETE &&
	    !state_is_retrain(exchange->state))
		clear_completion_tone(exchange, event);
	return true;
}

static bool retain_peer_info0(
	struct pcm_phase2_info0_exchange *exchange,
	const struct phase2_info0_exchange_event *raw,
	struct pcm_phase2_info0_exchange_event *event)
{
	union pcm_info0_record decoded = { 0 };
	bool retained = state_retains_peer_info0(exchange->state);
	bool decoded_ack;

	event->tone_event = raw->tone_event;
	event->protocol = exchange->protocol;
	if (raw->info_event.type == V34_PHASE2_INFO_NO_EVENT)
		return true;
	if (raw->info_event.type != V34_PHASE2_INFO_FRAME_RECEIVED)
		return reject_peer_info0(exchange, event);
	if (!decode_record(
		    exchange->protocol, opposite_role(exchange->local_role),
		    raw->info_event.payload, sizeof(raw->info_event.payload),
		    raw->info_event.payload_bit_count, &decoded))
		return reject_peer_info0(exchange, event);
	if (retained &&
	    !info0_semantics_match(exchange, &exchange->peer_info0, &decoded))
		return reject_peer_info0(exchange, event);
	decoded_ack = record_common_const(
		&decoded, exchange->protocol,
		opposite_role(exchange->local_role))
		->acknowledges_peer_info0;
	if (state_is_retrain(exchange->state))
		return true;
	if (exchange->state == PCM_PHASE2_INFO0_VERIFY_REPEAT) {
		record_common(
			&exchange->peer_info0, exchange->protocol,
			opposite_role(exchange->local_role))
			->acknowledges_peer_info0 = decoded_ack;
		clear_completion_tone(exchange, event);
		exchange->state = PCM_PHASE2_INFO0_ACKNOWLEDGING;
		return true;
	}
	if (!retained) {
		exchange->peer_info0 = decoded;
	} else
		record_common(
			&exchange->peer_info0, exchange->protocol,
			opposite_role(exchange->local_role))
			->acknowledges_peer_info0 |=
			record_common_const(
				&decoded, exchange->protocol,
				opposite_role(exchange->local_role))
					->acknowledges_peer_info0;
	if (exchange->state == PCM_PHASE2_INFO0_COMPLETE) {
		if (peer_acknowledges_local(exchange)) {
			exchange->fresh_peer_tone_seen = false;
			return true;
		}
		clear_completion_tone(exchange, event);
		exchange->state = PCM_PHASE2_INFO0_ACK_REPEAT;
		return true;
	}
	clear_completion_tone(exchange, event);
	switch (exchange->state) {
	case PCM_PHASE2_INFO0_INITIAL:
	case PCM_PHASE2_INFO0_REPEAT:
		exchange->state = PCM_PHASE2_INFO0_ACKNOWLEDGING;
		break;
	case PCM_PHASE2_INFO0_VERIFY_REPEAT:
		return false;
	case PCM_PHASE2_INFO0_ACKNOWLEDGING:
		if (!peer_acknowledges_local(exchange) &&
		    local_info0_acknowledges_peer(exchange))
			exchange->state = PCM_PHASE2_INFO0_ACK_REPEAT;
		break;
	case PCM_PHASE2_INFO0_ACK_REPEAT:
		if (peer_acknowledges_local(exchange))
			exchange->state = PCM_PHASE2_INFO0_ACKNOWLEDGING;
		break;
	case PCM_PHASE2_INFO0_WAIT_COMPLETION:
		if (!peer_acknowledges_local(exchange))
			exchange->state = PCM_PHASE2_INFO0_ACK_REPEAT;
		break;
	case PCM_PHASE2_INFO0_COMPLETE:
		return false;
	case PCM_PHASE2_INFO0_RETRAIN:
		return false;
	}
	return true;
}

static bool restart_local_info0(
	struct pcm_phase2_info0_exchange *exchange, bool acknowledge_peer)
{
	union pcm_info0_record local = { 0 };
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	const struct v34_phase2_info_generator *frame =
		&exchange->transport.info_transmitter.frame;
	struct v90_info0 *common;

	if (!decode_record(
		    exchange->protocol, exchange->local_role,
		    frame->payload, sizeof(frame->payload),
		    frame->payload_bit_count, &local))
		return false;
	common = record_common(
		&local, exchange->protocol, exchange->local_role);
	common->acknowledges_peer_info0 = acknowledge_peer;
	return encode_record(
		       exchange->protocol, exchange->local_role, &local,
		       payload, sizeof(payload)) &&
	       phase2_info0_exchange_restart_info(
		       &exchange->transport, payload,
		       payload_bits_for_role(exchange->local_role)) ==
		       PHASE2_INFO0_EXCHANGE_OK;
}

static bool begin_repeat_verification(
	struct pcm_phase2_info0_exchange *exchange,
	struct pcm_phase2_info0_exchange_event *event)
{
	record_common(
		&exchange->peer_info0, exchange->protocol,
		opposite_role(exchange->local_role))
		->acknowledges_peer_info0 = false;
	clear_completion_tone(exchange, event);
	if (!restart_local_info0(exchange, true))
		return false;
	exchange->state = PCM_PHASE2_INFO0_VERIFY_REPEAT;
	return true;
}

static void complete_exchange(
	struct pcm_phase2_info0_exchange *exchange,
	struct pcm_phase2_info0_exchange_event *event)
{
	exchange->state = PCM_PHASE2_INFO0_COMPLETE;
	event->peer_info0_event = PCM_PEER_INFO0_RECEIVED;
	event->peer_info0 = exchange->peer_info0;
}

static bool advance_recovery(
	struct pcm_phase2_info0_exchange *exchange,
	struct pcm_phase2_info0_exchange_event *event)
{
	if (event->tone_event.kind == V34_PHASE2_TONE_STARTED) {
		if (exchange->state == PCM_PHASE2_INFO0_INITIAL)
			exchange->state = PCM_PHASE2_INFO0_REPEAT;
		else if ((exchange->state == PCM_PHASE2_INFO0_ACKNOWLEDGING ||
			  exchange->state == PCM_PHASE2_INFO0_ACK_REPEAT) &&
			 !peer_acknowledges_local(exchange))
			exchange->fresh_peer_tone_seen = true;
	}
	if (state_is_retrain(exchange->state))
		return true;
	if (exchange->transport.transmit_phase != PHASE2_INFO0_TONE)
		return true;

	switch (exchange->state) {
	case PCM_PHASE2_INFO0_INITIAL:
		return true;
	case PCM_PHASE2_INFO0_REPEAT:
		return restart_local_info0(exchange, false);
	case PCM_PHASE2_INFO0_VERIFY_REPEAT:
		return true;
	case PCM_PHASE2_INFO0_ACKNOWLEDGING:
		if (!local_info0_acknowledges_peer(exchange))
			return restart_local_info0(exchange, true);
		if (peer_acknowledges_local(exchange) ||
		    exchange->fresh_peer_tone_seen)
			complete_exchange(exchange, event);
		else {
			clear_completion_tone(exchange, event);
			if (v34_phase2_tone_detector_reset(
				    &exchange->transport.tone_detector) !=
			    V34_PHASE2_TONE_OK)
				return false;
			exchange->state = PCM_PHASE2_INFO0_WAIT_COMPLETION;
		}
		return true;
	case PCM_PHASE2_INFO0_ACK_REPEAT:
		if (!restart_local_info0(exchange, true))
			return false;
		exchange->state = PCM_PHASE2_INFO0_ACKNOWLEDGING;
		return true;
	case PCM_PHASE2_INFO0_WAIT_COMPLETION:
		if (peer_acknowledges_local(exchange)) {
			complete_exchange(exchange, event);
			return true;
		}
		if (event->tone_event.kind != V34_PHASE2_TONE_STARTED)
			return true;
		exchange->fresh_peer_tone_seen = true;
		complete_exchange(exchange, event);
		return true;
	case PCM_PHASE2_INFO0_COMPLETE:
	case PCM_PHASE2_INFO0_RETRAIN:
		return true;
	}
	return false;
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_sample(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_info0_exchange_event *event)
{
	struct pcm_phase2_info0_exchange next;
	struct phase2_info0_exchange_event raw_event;
	struct pcm_phase2_info0_exchange_event emitted = { 0 };
	int16_t generated;
	bool info_started;

	if (!pcm_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info0_exchange_sample(
		    &next.transport, received_sample, &generated, &raw_event) !=
		    PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	info_started = exchange->transport.info_receiver.frame.stage ==
		       V34_PHASE2_INFO_SEARCH_SYNC &&
	       next.transport.info_receiver.frame.stage !=
		       V34_PHASE2_INFO_SEARCH_SYNC;
	if (info_started && next.state == PCM_PHASE2_INFO0_COMPLETE) {
		if (!begin_repeat_verification(&next, &emitted))
			return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	} else if (info_started) {
		next.fresh_peer_tone_seen = false;
	}
	if (!retain_peer_info0(&next, &raw_event, &emitted) ||
	    !advance_recovery(&next, &emitted))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	if (!pcm_phase2_info0_exchange_is_valid(&next))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return PCM_PHASE2_INFO0_EXCHANGE_OK;
}

const char *pcm_phase2_info0_exchange_result_name(
	enum pcm_phase2_info0_exchange_result result)
{
	switch (result) {
	case PCM_PHASE2_INFO0_EXCHANGE_OK:
		return "ok";
	case PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
