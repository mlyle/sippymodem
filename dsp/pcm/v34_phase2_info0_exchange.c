// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info0_exchange.h"

#include <string.h>

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static enum phase2_signal_role signal_for_station(
	enum v34_station_role role)
{
	return role == V34_CALLING_STATION ?
		       PHASE2_SIGNAL_B : PHASE2_SIGNAL_A;
}

bool v34_phase2_info0_exchange_is_valid(
	const struct v34_phase2_info0_exchange *exchange)
{
	return exchange != NULL && role_is_valid(exchange->local_role) &&
	       phase2_info0_exchange_is_valid(&exchange->transport) &&
	       exchange->transport.signal_role ==
		       signal_for_station(exchange->local_role) &&
	       v34_info0_is_valid(&exchange->local_info0) &&
	       exchange->local_info0.acknowledges_peer_info0 ==
		       exchange->peer_info0_received &&
	       (!exchange->peer_info0_received ||
		v34_info0_is_valid(&exchange->peer_info0)) &&
	       !(exchange->recovery_requested && exchange->recovery_active) &&
	       (!exchange->recovery_requested ||
		exchange->transport.transmit_phase != PHASE2_INFO0_TONE) &&
	       (!exchange->recovery_active ||
		exchange->transport.transmit_phase != PHASE2_INFO0_TONE) &&
	       (!exchange->transmit_acknowledges_peer ||
		exchange->recovery_active) &&
	       (!exchange->completion_tone_seen ||
		(exchange->acknowledgements_complete &&
		 exchange->peer_info0_received &&
		 exchange->local_info0.acknowledges_peer_info0)) &&
	       (!exchange->acknowledgements_complete ||
		(exchange->peer_info0_received &&
		 (exchange->peer_info0.acknowledges_peer_info0 ||
		  exchange->completion_tone_seen) &&
		 exchange->local_info0.acknowledges_peer_info0));
}

enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_init(
	struct v34_phase2_info0_exchange *exchange,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0)
{
	struct v34_phase2_info0_exchange initialized = {
		.local_role = local_role,
	};
	struct v34_info0 initial_info0;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];

	if (exchange == NULL || !role_is_valid(local_role) ||
	    !v34_info0_is_valid(local_info0))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial_info0 = *local_info0;
	initial_info0.acknowledges_peer_info0 = false;
	initialized.local_info0 = initial_info0;
	if (v34_info0_encode(&initial_info0, payload, sizeof(payload)) !=
		    V34_INFO0_OK ||
	    phase2_info0_exchange_init(
		    &initialized.transport, signal_for_station(local_role),
		    nominal_amplitude, payload, V34_INFO0_PAYLOAD_BITS,
		    V34_INFO0_PAYLOAD_BITS) != PHASE2_INFO0_EXCHANGE_OK ||
	    !v34_phase2_info0_exchange_is_valid(&initialized))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return V34_PHASE2_INFO0_EXCHANGE_OK;
}

enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_init_retrain(
	struct v34_phase2_info0_exchange *exchange,
	enum v34_station_role local_role, int16_t nominal_amplitude,
	const struct v34_info0 *local_info0,
	const struct v34_info0 *peer_info0)
{
	struct v34_phase2_info0_exchange initialized;

	if (exchange == NULL || !v34_info0_is_valid(peer_info0) ||
	    v34_phase2_info0_exchange_init(
		    &initialized, local_role, nominal_amplitude,
		    local_info0) != V34_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_info0_exchange_start_tone(
		    &initialized.transport) != PHASE2_INFO0_EXCHANGE_OK)
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initialized.local_info0.acknowledges_peer_info0 = true;
	initialized.peer_info0 = *peer_info0;
	initialized.peer_info0_received = true;
	initialized.acknowledgements_complete = true;
	initialized.completion_tone_seen = true;
	if (!v34_phase2_info0_exchange_is_valid(&initialized))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return V34_PHASE2_INFO0_EXCHANGE_OK;
}

static bool info0_semantics_match(
	const struct v34_info0 *left, const struct v34_info0 *right)
{
	struct v34_info0 left_without_ack = *left;
	struct v34_info0 right_without_ack = *right;
	uint8_t left_payload[V34_INFO0_PAYLOAD_BYTES];
	uint8_t right_payload[V34_INFO0_PAYLOAD_BYTES];

	left_without_ack.acknowledges_peer_info0 = false;
	right_without_ack.acknowledges_peer_info0 = false;
	return v34_info0_encode(
		       &left_without_ack, left_payload,
		       sizeof(left_payload)) == V34_INFO0_OK &&
	       v34_info0_encode(
		       &right_without_ack, right_payload,
		       sizeof(right_payload)) == V34_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool decode_event(
	struct v34_phase2_info0_exchange *exchange,
	const struct phase2_info0_exchange_event *raw,
	struct v34_phase2_info0_exchange_event *event)
{
	struct v34_info0 decoded;
	bool repeated;

	event->tone_event = raw->tone_event;
	if (raw->info_event.type == V34_PHASE2_INFO_NO_EVENT)
		return false;
	if (raw->info_event.type == V34_PHASE2_INFO_FRAME_REJECTED) {
		event->peer_info0_event = V34_PHASE2_PEER_INFO0_REJECTED;
		return false;
	}
	if (raw->info_event.type != V34_PHASE2_INFO_FRAME_RECEIVED ||
	    v34_info0_decode(
		    raw->info_event.payload, sizeof(raw->info_event.payload),
		    raw->info_event.payload_bit_count, &decoded) != V34_INFO0_OK) {
		event->peer_info0_event = V34_PHASE2_PEER_INFO0_REJECTED;
		return false;
	}
	repeated = exchange->peer_info0_received;
	if (repeated &&
	    !info0_semantics_match(&exchange->peer_info0, &decoded)) {
		event->peer_info0_event = V34_PHASE2_PEER_INFO0_REJECTED;
		return false;
	}
	if (!repeated)
		exchange->peer_info0 = decoded;
	else
		exchange->peer_info0.acknowledges_peer_info0 |=
			decoded.acknowledges_peer_info0;
	exchange->peer_info0_received = true;
	exchange->local_info0.acknowledges_peer_info0 = true;
	event->peer_info0_event = V34_PHASE2_PEER_INFO0_RECEIVED;
	event->peer_info0 = exchange->peer_info0;
	return repeated;
}

static bool restart_local_info0(
	struct v34_phase2_info0_exchange *exchange)
{
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];

	if (v34_info0_encode(
		    &exchange->local_info0, payload, sizeof(payload)) !=
		    V34_INFO0_OK ||
	    phase2_info0_exchange_restart_info(
		    &exchange->transport, payload,
		    V34_INFO0_PAYLOAD_BITS) != PHASE2_INFO0_EXCHANGE_OK)
		return false;
	exchange->recovery_requested = false;
	exchange->recovery_active = true;
	exchange->transmit_acknowledges_peer =
		exchange->local_info0.acknowledges_peer_info0;
	return true;
}

static bool advance_recovery(
	struct v34_phase2_info0_exchange *exchange,
	bool repeated_peer_info0,
	struct v34_phase2_info0_exchange_event *event)
{
	bool peer_tone_before_info =
		event->tone_event.kind == V34_PHASE2_TONE_STARTED &&
		!exchange->peer_info0_received;

	if ((repeated_peer_info0 || peer_tone_before_info) &&
	    !exchange->acknowledgements_complete &&
	    !exchange->recovery_active)
		exchange->recovery_requested = true;
	if (exchange->transport.transmit_phase != PHASE2_INFO0_TONE)
		return true;
	/* A peer acknowledgment or its following tone completes recovery.  The
	 * latter is the normative alternative when the final acknowledged INFO0
	 * frame is missed: finish the current local acknowledgment, then proceed
	 * with ranging instead of retransmitting forever. */
	if (exchange->recovery_active && exchange->peer_info0_received &&
	    (exchange->peer_info0.acknowledges_peer_info0 ||
	     exchange->transport.tone_detector.tone_present) &&
	    exchange->transmit_acknowledges_peer) {
		exchange->recovery_active = false;
		exchange->transmit_acknowledges_peer = false;
		exchange->acknowledgements_complete = true;
		exchange->completion_tone_seen =
			!exchange->peer_info0.acknowledges_peer_info0;
		event->recovery_completed = true;
		return true;
	}
	if (!exchange->recovery_active && !exchange->recovery_requested)
		return true;
	event->recovery_started = !exchange->recovery_active;
	return restart_local_info0(exchange);
}

enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_sample(
	struct v34_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info0_exchange_event *event)
{
	struct v34_phase2_info0_exchange next;
	struct phase2_info0_exchange_event raw_event;
	struct v34_phase2_info0_exchange_event emitted = { 0 };
	int16_t generated;
	bool repeated_peer_info0;

	if (!v34_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info0_exchange_sample(
		    &next.transport, received_sample, &generated, &raw_event) !=
		    PHASE2_INFO0_EXCHANGE_OK)
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	repeated_peer_info0 = decode_event(&next, &raw_event, &emitted);
	if (!advance_recovery(
		    &next, repeated_peer_info0, &emitted))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	if (!v34_phase2_info0_exchange_is_valid(&next))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_INFO0_EXCHANGE_OK;
}

const char *v34_phase2_info0_exchange_result_name(
	enum v34_phase2_info0_exchange_result result)
{
	switch (result) {
	case V34_PHASE2_INFO0_EXCHANGE_OK:
		return "ok";
	case V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
