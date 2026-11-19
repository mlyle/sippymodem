// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info1_exchange.h"

#include <stddef.h>

bool v34_phase2_info1_exchange_is_valid(
	const struct v34_phase2_info1_exchange *exchange)
{
	bool expected;

	if (exchange == NULL || !phase2_info1_is_valid(&exchange->control) ||
	    exchange->control.short_exchange ||
	    exchange->control.phase == PHASE2_INFO1_PEER_FRAME_PENDING)
		return false;
	expected = exchange->control.signal_role == PHASE2_SIGNAL_B ?
		phase2_info1_has_second_record(&exchange->control) :
		phase2_info1_has_first_record(&exchange->control);
	if (exchange->peer_parameters_received != expected)
		return false;
	if (!expected)
		return true;
	return exchange->control.signal_role == PHASE2_SIGNAL_B ?
		       v34_info1a_is_valid(&exchange->peer_parameters.info1a) :
		       v34_info1c_is_valid(&exchange->peer_parameters.info1c);
}

enum phase2_info1_result v34_phase2_info1_exchange_caller_init(
	struct v34_phase2_info1_exchange *exchange,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples,
	const struct v34_info1c *local_info1c)
{
	struct v34_phase2_info1_exchange initialized = { 0 };
	uint8_t payload[V34_INFO1C_PAYLOAD_BYTES];

	if (exchange == NULL ||
	    v34_info1c_encode(local_info1c, payload, sizeof(payload)) !=
		    V34_INFO1_OK ||
	    phase2_info1_init_signal_b(
		    &initialized.control, nominal_amplitude,
		    round_trip_delay_samples, payload,
		    V34_INFO1C_PAYLOAD_BITS, V34_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !v34_phase2_info1_exchange_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result v34_phase2_info1_exchange_answerer_init(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_phase2_tone_transmitter *tone,
	uint16_t round_trip_delay_samples, uint32_t reversal_elapsed_samples)
{
	struct v34_phase2_info1_exchange initialized = { 0 };

	if (exchange == NULL || tone == NULL)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	if (phase2_info1_init_signal_a(
		    &initialized.control, tone,
		    v34_phase2_tone_nominal_amplitude(tone),
		    round_trip_delay_samples, reversal_elapsed_samples,
		    V34_INFO1C_PAYLOAD_BITS, V34_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !v34_phase2_info1_exchange_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result v34_phase2_info1_exchange_answerer_reply(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_info1a *local_info1a)
{
	struct v34_phase2_info1_exchange next;
	uint8_t payload[V34_INFO1A_PAYLOAD_BYTES];

	if (!v34_phase2_info1_exchange_is_valid(exchange) ||
	    exchange->control.signal_role != PHASE2_SIGNAL_A ||
	    v34_info1a_encode(local_info1a, payload, sizeof(payload)) !=
		    V34_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info1_reply_signal_a(
		    &next.control, payload, V34_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !v34_phase2_info1_exchange_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

static bool resolve_peer_frame(
	struct v34_phase2_info1_exchange *exchange,
	struct v34_phase2_info1_exchange_event *event)
{
	const struct v34_phase2_info_event *frame = &event->transport.frame_event;
	union v34_phase2_info1_parameters decoded = { 0 };
	bool valid;

	if (exchange->control.signal_role == PHASE2_SIGNAL_B)
		valid = v34_info1a_decode(
				frame->payload, sizeof(frame->payload),
				frame->payload_bit_count, &decoded.info1a) ==
			V34_INFO1_OK;
	else
		valid = v34_info1c_decode(
				frame->payload, sizeof(frame->payload),
				frame->payload_bit_count, &decoded.info1c) ==
			V34_INFO1_OK;
	if (phase2_info1_resolve_peer(&exchange->control, &event->transport, valid) !=
	    PHASE2_INFO1_OK)
		return false;
	if (valid) {
		exchange->peer_parameters = decoded;
		exchange->peer_parameters_received = true;
		event->peer_parameters = decoded;
	}
	return true;
}

enum phase2_info1_result v34_phase2_info1_exchange_sample(
	struct v34_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info1_exchange_event *event)
{
	struct v34_phase2_info1_exchange next;
	struct v34_phase2_info1_exchange_event emitted = { 0 };
	int16_t generated;

	if (!v34_phase2_info1_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info1_sample_owned(
		    &next.control, received_sample, &generated, &emitted.transport) !=
	    PHASE2_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	switch (emitted.transport.kind) {
	case PHASE2_INFO1_EVENT_PEER_FRAME_PENDING:
		if (!resolve_peer_frame(&next, &emitted))
			return PHASE2_INFO1_INVALID_ARGUMENT;
		break;
	case PHASE2_INFO1_EVENT_PEER_INFOMARKS:
		/* Retain V.34 INFO1c resend policy in the mode owner. */
		if (next.control.signal_role == PHASE2_SIGNAL_B &&
		    next.control.phase == PHASE2_INFO1_RECOVERING &&
		    phase2_info1_repeat_signal_b(&next.control) != PHASE2_INFO1_OK)
			return PHASE2_INFO1_INVALID_ARGUMENT;
		/* fall through */
	case PHASE2_INFO1_EVENT_PEER_FRAME_STARTED:
		emitted.transport.kind = PHASE2_INFO1_EVENT_NONE;
		break;
	default:
		break;
	}
	if (!v34_phase2_info1_exchange_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INFO1_OK;
}
