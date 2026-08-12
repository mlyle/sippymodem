// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_info0_exchange.h"

static bool role_is_valid(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM || role == V90_DIGITAL_MODEM;
}

static enum phase2_signal_role signal_for_role(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ? PHASE2_SIGNAL_A : PHASE2_SIGNAL_B;
}

static size_t local_payload_bits(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ?
		       V90_INFO0A_PAYLOAD_BITS : V90_INFO0D_PAYLOAD_BITS;
}

static size_t peer_payload_bits(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM ?
		       V90_INFO0D_PAYLOAD_BITS : V90_INFO0A_PAYLOAD_BITS;
}

static bool peer_info0_is_valid(
	const struct v90_phase2_info0_exchange *exchange)
{
	if (!exchange->peer_info0_received)
		return true;
	return exchange->local_role == V90_ANALOG_MODEM ?
		       v90_info0d_is_valid(&exchange->peer_info0.digital) :
		       v90_info0_is_valid(&exchange->peer_info0.analog);
}

bool v90_phase2_info0_exchange_is_valid(
	const struct v90_phase2_info0_exchange *exchange)
{
	return exchange != NULL && role_is_valid(exchange->local_role) &&
	       phase2_info0_exchange_is_valid(&exchange->transport) &&
	       exchange->transport.signal_role ==
		       signal_for_role(exchange->local_role) &&
	       exchange->transport.info_transmitter.frame.payload_bit_count ==
		       local_payload_bits(exchange->local_role) &&
	       exchange->transport.info_receiver.frame
		       .expected_payload_bit_count ==
		       peer_payload_bits(exchange->local_role) &&
	       peer_info0_is_valid(exchange);
}

static enum v90_phase2_info0_exchange_result initialize(
	struct v90_phase2_info0_exchange *exchange,
	enum v90_modem_role role, int16_t nominal_amplitude,
	const uint8_t *payload)
{
	struct v90_phase2_info0_exchange initialized = {
		.local_role = role,
	};

	if (exchange == NULL || payload == NULL || !role_is_valid(role) ||
	    phase2_info0_exchange_init(
		    &initialized.transport, signal_for_role(role),
		    nominal_amplitude, payload, local_payload_bits(role),
		    peer_payload_bits(role)) != PHASE2_INFO0_EXCHANGE_OK ||
	    !v90_phase2_info0_exchange_is_valid(&initialized))
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return V90_PHASE2_INFO0_EXCHANGE_OK;
}

enum v90_phase2_info0_exchange_result
v90_phase2_info0_exchange_init_analog(
	struct v90_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a)
{
	struct v90_info0 initial;
	uint8_t payload[V90_INFO0A_PAYLOAD_BYTES];

	if (!v90_info0_is_valid(local_info0a))
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0a;
	initial.acknowledges_peer_info0 = false;
	if (v90_info0a_encode(&initial, payload, sizeof(payload)) !=
	    V90_INFO0_OK)
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	return initialize(
		exchange, V90_ANALOG_MODEM, nominal_amplitude, payload);
}

enum v90_phase2_info0_exchange_result
v90_phase2_info0_exchange_init_digital(
	struct v90_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	struct v90_info0d initial;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];

	if (!v90_info0d_is_valid(local_info0d))
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0d;
	initial.common.acknowledges_peer_info0 = false;
	if (v90_info0d_encode(&initial, payload, sizeof(payload)) !=
	    V90_INFO0_OK)
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	return initialize(
		exchange, V90_DIGITAL_MODEM, nominal_amplitude, payload);
}

static void decode_event(
	struct v90_phase2_info0_exchange *exchange,
	const struct phase2_info0_exchange_event *raw,
	struct v90_phase2_info0_exchange_event *event)
{
	union v90_info0_record decoded = { 0 };
	enum v90_info0_result result;

	event->tone_event = raw->tone_event;
	if (raw->info_event.type == V34_PHASE2_INFO_NO_EVENT)
		return;
	if (raw->info_event.type == V34_PHASE2_INFO_FRAME_REJECTED) {
		event->peer_info0_event = V90_PEER_INFO0_REJECTED;
		return;
	}
	if (raw->info_event.type != V34_PHASE2_INFO_FRAME_RECEIVED) {
		event->peer_info0_event = V90_PEER_INFO0_REJECTED;
		return;
	}
	if (exchange->local_role == V90_ANALOG_MODEM)
		result = v90_info0d_decode(
			raw->info_event.payload, sizeof(raw->info_event.payload),
			raw->info_event.payload_bit_count, &decoded.digital);
	else
		result = v90_info0a_decode(
			raw->info_event.payload, sizeof(raw->info_event.payload),
			raw->info_event.payload_bit_count, &decoded.analog);
	if (result != V90_INFO0_OK) {
		event->peer_info0_event = V90_PEER_INFO0_REJECTED;
		return;
	}
	exchange->peer_info0 = decoded;
	exchange->peer_info0_received = true;
	event->peer_info0_event = V90_PEER_INFO0_RECEIVED;
	event->peer_info0 = decoded;
}

enum v90_phase2_info0_exchange_result
v90_phase2_info0_exchange_sample(
	struct v90_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v90_phase2_info0_exchange_event *event)
{
	struct v90_phase2_info0_exchange next;
	struct phase2_info0_exchange_event raw_event;
	struct v90_phase2_info0_exchange_event emitted = { 0 };
	int16_t generated;

	if (!v90_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info0_exchange_sample(
		    &next.transport, received_sample, &generated, &raw_event) !=
	    PHASE2_INFO0_EXCHANGE_OK)
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	decode_event(&next, &raw_event, &emitted);
	if (!v90_phase2_info0_exchange_is_valid(&next))
		return V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return V90_PHASE2_INFO0_EXCHANGE_OK;
}

const char *v90_phase2_info0_exchange_result_name(
	enum v90_phase2_info0_exchange_result result)
{
	switch (result) {
	case V90_PHASE2_INFO0_EXCHANGE_OK:
		return "ok";
	case V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
