// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_info0_exchange.h"

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
	const struct pcm_phase2_info0_exchange *exchange)
{
	if (!exchange->peer_info0_received)
		return true;
	if (exchange->protocol == PCM_PROTOCOL_V90)
		return exchange->local_role == V90_ANALOG_MODEM ?
			       v90_info0d_is_valid(
				       &exchange->peer_info0.digital) :
			       v90_info0_is_valid(
				       &exchange->peer_info0.analog);
	return exchange->local_role == V90_ANALOG_MODEM ?
		       v92_info0d_is_valid(&exchange->peer_info0.v92_digital) :
		       v92_info0a_is_valid(&exchange->peer_info0.v92_analog);
}

bool pcm_phase2_info0_exchange_is_valid(
	const struct pcm_phase2_info0_exchange *exchange)
{
	return exchange != NULL &&
	       pcm_modem_role_is_valid(exchange->local_role) &&
	       pcm_protocol_is_valid(exchange->protocol) &&
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

static enum pcm_phase2_info0_exchange_result initialize(
	struct pcm_phase2_info0_exchange *exchange,
	enum pcm_protocol protocol, enum v90_modem_role role,
	int16_t nominal_amplitude,
	const uint8_t *payload)
{
	struct pcm_phase2_info0_exchange initialized = {
		.local_role = role,
		.protocol = protocol,
	};

	if (exchange == NULL || payload == NULL ||
	    !pcm_protocol_is_valid(protocol) ||
	    !pcm_modem_role_is_valid(role) ||
	    phase2_info0_exchange_init(
		    &initialized.transport, signal_for_role(role),
		    nominal_amplitude, payload, local_payload_bits(role),
		    peer_payload_bits(role)) != PHASE2_INFO0_EXCHANGE_OK ||
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
	struct v90_info0 initial;
	uint8_t payload[V90_INFO0A_PAYLOAD_BYTES];

	if (!v90_info0_is_valid(local_info0a))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0a;
	initial.acknowledges_peer_info0 = false;
	if (v90_info0a_encode(&initial, payload, sizeof(payload)) !=
	    V90_INFO0_OK)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	return initialize(
		exchange, PCM_PROTOCOL_V90, V90_ANALOG_MODEM,
		nominal_amplitude, payload);
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	struct v90_info0d initial;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];

	if (!v90_info0d_is_valid(local_info0d))
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0d;
	initial.common.acknowledges_peer_info0 = false;
	if (v90_info0d_encode(&initial, payload, sizeof(payload)) !=
	    V90_INFO0_OK)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	return initialize(
		exchange, PCM_PROTOCOL_V90, V90_DIGITAL_MODEM,
		nominal_amplitude, payload);
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a)
{
	struct v92_info0a initial;
	uint8_t payload[V90_INFO0A_PAYLOAD_BYTES];

	if (!v92_info0a_is_valid(local_info0a) || !local_info0a->v92_capable)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0a;
	initial.v90.acknowledges_peer_info0 = false;
	if (v92_info0a_encode(&initial, payload, sizeof(payload)) !=
	    V92_INFO0_OK)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	return initialize(
		exchange, PCM_PROTOCOL_V92, V90_ANALOG_MODEM,
		nominal_amplitude, payload);
}

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d)
{
	struct v92_info0d initial;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];

	if (!v92_info0d_is_valid(local_info0d) || !local_info0d->v92_capable)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial = *local_info0d;
	initial.v90.common.acknowledges_peer_info0 = false;
	if (v92_info0d_encode(&initial, payload, sizeof(payload)) !=
	    V92_INFO0_OK)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	return initialize(
		exchange, PCM_PROTOCOL_V92, V90_DIGITAL_MODEM,
		nominal_amplitude, payload);
}

static bool decode_peer_info0(
	const struct pcm_phase2_info0_exchange *exchange,
	const struct v34_phase2_info_event *raw,
	union pcm_info0_record *decoded)
{
	if (exchange->protocol == PCM_PROTOCOL_V90)
		return exchange->local_role == V90_ANALOG_MODEM ?
			       v90_info0d_decode(
				       raw->payload, sizeof(raw->payload),
				       raw->payload_bit_count,
				       &decoded->digital) == V90_INFO0_OK :
			       v90_info0a_decode(
				       raw->payload, sizeof(raw->payload),
				       raw->payload_bit_count,
				       &decoded->analog) == V90_INFO0_OK;
	return exchange->local_role == V90_ANALOG_MODEM ?
		       v92_info0d_decode(
			       raw->payload, sizeof(raw->payload),
			       raw->payload_bit_count,
			       &decoded->v92_digital) == V92_INFO0_OK :
		       v92_info0a_decode(
			       raw->payload, sizeof(raw->payload),
			       raw->payload_bit_count,
			       &decoded->v92_analog) == V92_INFO0_OK;
}

static void decode_event(
	struct pcm_phase2_info0_exchange *exchange,
	const struct phase2_info0_exchange_event *raw,
	struct pcm_phase2_info0_exchange_event *event)
{
	union pcm_info0_record decoded = { 0 };

	event->tone_event = raw->tone_event;
	event->protocol = exchange->protocol;
	if (raw->info_event.type == V34_PHASE2_INFO_NO_EVENT)
		return;
	if (raw->info_event.type == V34_PHASE2_INFO_FRAME_REJECTED) {
		event->peer_info0_event = PCM_PEER_INFO0_REJECTED;
		return;
	}
	if (raw->info_event.type != V34_PHASE2_INFO_FRAME_RECEIVED) {
		event->peer_info0_event = PCM_PEER_INFO0_REJECTED;
		return;
	}
	if (!decode_peer_info0(exchange, &raw->info_event, &decoded)) {
		event->peer_info0_event = PCM_PEER_INFO0_REJECTED;
		return;
	}
	exchange->peer_info0 = decoded;
	exchange->peer_info0_received = true;
	event->peer_info0_event = PCM_PEER_INFO0_RECEIVED;
	event->peer_info0 = decoded;
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

	if (!pcm_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info0_exchange_sample(
		    &next.transport, received_sample, &generated, &raw_event) !=
	    PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	decode_event(&next, &raw_event, &emitted);
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
