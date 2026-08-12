// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info0_exchange.h"

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
	       (!exchange->peer_info0_received ||
		v34_info0_is_valid(&exchange->peer_info0));
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

static void decode_event(
	struct v34_phase2_info0_exchange *exchange,
	const struct phase2_info0_exchange_event *raw,
	struct v34_phase2_info0_exchange_event *event)
{
	struct v34_info0 decoded;

	event->tone_event = raw->tone_event;
	if (raw->info_event.type == V34_PHASE2_INFO_NO_EVENT)
		return;
	if (raw->info_event.type == V34_PHASE2_INFO_FRAME_REJECTED) {
		event->peer_info0_event = V34_PHASE2_PEER_INFO0_REJECTED;
		return;
	}
	if (raw->info_event.type != V34_PHASE2_INFO_FRAME_RECEIVED ||
	    v34_info0_decode(
		    raw->info_event.payload, sizeof(raw->info_event.payload),
		    raw->info_event.payload_bit_count, &decoded) != V34_INFO0_OK) {
		event->peer_info0_event = V34_PHASE2_PEER_INFO0_REJECTED;
		return;
	}
	exchange->peer_info0 = decoded;
	exchange->peer_info0_received = true;
	event->peer_info0_event = V34_PHASE2_PEER_INFO0_RECEIVED;
	event->peer_info0 = decoded;
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

	if (!v34_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info0_exchange_sample(
		    &next.transport, received_sample, &generated, &raw_event) !=
		    PHASE2_INFO0_EXCHANGE_OK)
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	decode_event(&next, &raw_event, &emitted);
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
