// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info1_exchange.h"

#include <stddef.h>

static enum v34_station_role probe_role(
	const struct v34_phase2_reciprocal_probe *probe)
{
	return probe->turnaround.initial_probe.ranging.info0_exchange.local_role;
}

static bool completed_probe_matches_role(
	const struct v34_phase2_reciprocal_probe *probe)
{
	return probe_role(probe) == V34_CALLING_STATION ?
		       probe->control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE :
		       probe->control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE;
}

static int16_t probe_nominal_amplitude(
	const struct v34_phase2_reciprocal_probe *probe)
{
	const struct v34_phase2_tone_transmitter *tone =
		&probe->turnaround.initial_probe.ranging.info0_exchange
			 .transport.tone_transmitter;

	return probe_role(probe) == V34_CALLING_STATION ?
		       tone->carrier_amplitude : tone->guard_amplitude;
}

static uint16_t probe_round_trip_delay(
	const struct v34_phase2_reciprocal_probe *probe)
{
	return probe->turnaround.initial_probe.ranging.control
		.round_trip_delay_samples;
}

static bool phase_has_peer_parameters(
	const struct v34_phase2_info1_exchange *exchange)
{
	const struct phase2_info1_exchange *control = &exchange->control;

	if (control->signal_role == PHASE2_SIGNAL_B)
		return control->phase == PHASE2_INFO1_COMPLETE;
	return control->phase == PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED ||
	       control->phase == PHASE2_INFO1_TRANSMITTING ||
	       control->phase == PHASE2_INFO1_CARRIER_TAIL ||
	       control->phase == PHASE2_INFO1_COMPLETE;
}

bool v34_phase2_info1_exchange_is_valid(
	const struct v34_phase2_info1_exchange *exchange)
{
	bool expected;

	if (exchange == NULL || !phase2_info1_is_valid(&exchange->control) ||
	    exchange->control.phase == PHASE2_INFO1_PEER_FRAME_PENDING)
		return false;
	expected = phase_has_peer_parameters(exchange);
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
	const struct v34_phase2_reciprocal_probe *completed_probe,
	const struct v34_info1c *local_info1c)
{
	struct v34_phase2_info1_exchange initialized = { 0 };
	uint8_t payload[V34_INFO1C_PAYLOAD_BYTES];

	if (exchange == NULL ||
	    !v34_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) != V34_CALLING_STATION ||
	    v34_info1c_encode(local_info1c, payload, sizeof(payload)) !=
		    V34_INFO1_OK ||
	    phase2_info1_init_signal_b(
		    &initialized.control,
		    probe_nominal_amplitude(completed_probe),
		    probe_round_trip_delay(completed_probe), payload,
		    V34_INFO1C_PAYLOAD_BITS, V34_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !v34_phase2_info1_exchange_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result v34_phase2_info1_exchange_answerer_init(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_phase2_reciprocal_probe *completed_probe)
{
	struct v34_phase2_info1_exchange initialized = { 0 };
	const struct v34_phase2_tone_transmitter *tone;

	if (exchange == NULL ||
	    !v34_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) != V34_ANSWERING_STATION)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	tone = &completed_probe->turnaround.initial_probe.ranging.info0_exchange
			.transport.tone_transmitter;
	if (phase2_info1_init_signal_a(
		    &initialized.control, tone,
		    probe_nominal_amplitude(completed_probe),
		    probe_round_trip_delay(completed_probe),
		    completed_probe->control.signal_a_reversal_elapsed_samples,
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

static bool reject_peer_frame(
	struct v34_phase2_info1_exchange *exchange,
	struct v34_phase2_info1_exchange_event *event)
{
	if (phase2_info1_reject_peer(&exchange->control) != PHASE2_INFO1_OK)
		return false;
	if (exchange->control.phase == PHASE2_INFO1_FAILED) {
		event->kind = V34_PHASE2_INFO1_EVENT_FAILED;
		event->failure = exchange->control.failure;
	} else {
		event->kind = V34_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
	}
	return true;
}

static bool resolve_peer_frame(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_phase2_info_event *frame,
	struct v34_phase2_info1_exchange_event *event)
{
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
	if (!valid)
		return reject_peer_frame(exchange, event);
	if (phase2_info1_accept_peer(&exchange->control) != PHASE2_INFO1_OK)
		return false;
	exchange->peer_parameters = decoded;
	exchange->peer_parameters_received = true;
	event->peer_parameters = decoded;
	event->kind = exchange->control.signal_role == PHASE2_SIGNAL_B ?
			      V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED :
			      V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED;
	return true;
}

static bool map_common_event(
	struct v34_phase2_info1_exchange *exchange,
	const struct phase2_info1_event *common,
	struct v34_phase2_info1_exchange_event *event)
{
	event->frame_event = common->frame_event;
	switch (common->kind) {
	case PHASE2_INFO1_EVENT_NONE:
		return true;
	case PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT:
		event->kind = V34_PHASE2_INFO1_EVENT_LOCAL_INFO1C_SENT;
		return true;
	case PHASE2_INFO1_EVENT_PEER_FRAME_STARTED:
		return true;
	case PHASE2_INFO1_EVENT_PEER_FRAME_PENDING:
		return resolve_peer_frame(exchange, &common->frame_event, event);
	case PHASE2_INFO1_EVENT_LOCAL_SECOND_SENT:
		event->kind = V34_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT;
		return true;
	case PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED:
		event->kind = V34_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
		return true;
	case PHASE2_INFO1_EVENT_FAILED:
		event->kind = V34_PHASE2_INFO1_EVENT_FAILED;
		event->failure = common->failure;
		return true;
	}
	return false;
}

enum phase2_info1_result v34_phase2_info1_exchange_sample(
	struct v34_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info1_exchange_event *event)
{
	struct v34_phase2_info1_exchange next;
	struct v34_phase2_info1_exchange_event emitted = { 0 };
	struct phase2_info1_event common;
	int16_t generated;

	if (!v34_phase2_info1_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info1_sample(
		    &next.control, received_sample, &generated, &common) !=
		    PHASE2_INFO1_OK ||
	    !map_common_event(&next, &common, &emitted) ||
	    !v34_phase2_info1_exchange_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INFO1_OK;
}
