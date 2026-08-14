// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_info1_exchange.h"

#include <stddef.h>

static enum v90_modem_role probe_role(
	const struct v90_phase2_reciprocal_probe *probe)
{
	return probe->turnaround.initial_probe.ranging.info0_exchange.local_role;
}

static bool completed_probe_matches_role(
	const struct v90_phase2_reciprocal_probe *probe)
{
	return probe_role(probe) == V90_DIGITAL_MODEM ?
		       probe->control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE :
		       probe->control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE;
}

static const struct v34_phase2_tone_transmitter *probe_tone(
	const struct v90_phase2_reciprocal_probe *probe)
{
	return &probe->turnaround.initial_probe.ranging.info0_exchange.transport
			.tone_transmitter;
}

static int16_t probe_nominal_amplitude(
	const struct v90_phase2_reciprocal_probe *probe)
{
	const struct v34_phase2_tone_transmitter *tone = probe_tone(probe);

	return probe_role(probe) == V90_DIGITAL_MODEM ?
		       tone->carrier_amplitude : tone->guard_amplitude;
}

static uint16_t probe_round_trip_delay(
	const struct v90_phase2_reciprocal_probe *probe)
{
	return probe->turnaround.initial_probe.ranging.control
		.round_trip_delay_samples;
}

static bool phase_has_peer_record(
	const struct pcm_phase2_info1_exchange *exchange)
{
	const struct phase2_info1_exchange *control = &exchange->control;

	if (control->signal_role == PHASE2_SIGNAL_B)
		return control->phase == PHASE2_INFO1_COMPLETE;
	return control->phase == PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED ||
	       control->phase == PHASE2_INFO1_TRANSMITTING ||
	       control->phase == PHASE2_INFO1_CARRIER_TAIL ||
	       control->phase == PHASE2_INFO1_COMPLETE;
}

bool pcm_phase2_info1_is_valid(
	const struct pcm_phase2_info1_exchange *exchange)
{
	bool expected;

	if (exchange == NULL || !phase2_info1_is_valid(&exchange->control) ||
	    exchange->control.phase == PHASE2_INFO1_PEER_FRAME_PENDING)
		return false;
	expected = phase_has_peer_record(exchange);
	if (exchange->peer_record_received != expected)
		return false;
	if (!expected)
		return true;
	return exchange->control.signal_role == PHASE2_SIGNAL_B ?
		       v90_info1a_is_valid(&exchange->peer_record.info1a) :
		       v34_info1c_is_valid(&exchange->peer_record.info1d);
}

enum phase2_info1_result pcm_phase2_info1_init_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct v34_info1c *local_info1d)
{
	struct pcm_phase2_info1_exchange initialized = { 0 };
	uint8_t payload[V34_INFO1C_PAYLOAD_BYTES];

	if (exchange == NULL ||
	    !v90_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) != V90_DIGITAL_MODEM ||
	    v34_info1c_encode(local_info1d, payload, sizeof(payload)) !=
		    V34_INFO1_OK ||
	    phase2_info1_init_signal_b(
		    &initialized.control,
		    probe_nominal_amplitude(completed_probe),
		    probe_round_trip_delay(completed_probe), payload,
		    V34_INFO1C_PAYLOAD_BITS, V90_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !pcm_phase2_info1_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result pcm_phase2_info1_init_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe)
{
	struct pcm_phase2_info1_exchange initialized = { 0 };

	if (exchange == NULL ||
	    !v90_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) != V90_ANALOG_MODEM ||
	    phase2_info1_init_signal_a(
		    &initialized.control, probe_tone(completed_probe),
		    probe_nominal_amplitude(completed_probe),
		    probe_round_trip_delay(completed_probe),
		    completed_probe->control.signal_a_reversal_elapsed_samples,
		    V34_INFO1C_PAYLOAD_BITS, V90_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !pcm_phase2_info1_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result pcm_phase2_info1_reply_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_info1a *local_info1a)
{
	struct pcm_phase2_info1_exchange next;
	uint8_t payload[V90_INFO1A_PAYLOAD_BYTES];

	if (!pcm_phase2_info1_is_valid(exchange) ||
	    exchange->control.signal_role != PHASE2_SIGNAL_A ||
	    v90_info1a_encode(local_info1a, payload, sizeof(payload)) !=
		    V90_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info1_reply_signal_a(
		    &next.control, payload, V90_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK ||
	    !pcm_phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

static bool reject_peer_frame(
	struct pcm_phase2_info1_exchange *exchange,
	struct pcm_phase2_info1_event *event)
{
	if (phase2_info1_reject_peer(&exchange->control) != PHASE2_INFO1_OK)
		return false;
	if (exchange->control.phase == PHASE2_INFO1_FAILED) {
		event->kind = PCM_PHASE2_INFO1_EVENT_FAILED;
		event->failure = exchange->control.failure;
	} else {
		event->kind = PCM_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
	}
	return true;
}

static bool resolve_peer_frame(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v34_phase2_info_event *frame,
	struct pcm_phase2_info1_event *event)
{
	union pcm_info1_record decoded = { 0 };
	bool valid;

	if (exchange->control.signal_role == PHASE2_SIGNAL_B)
		valid = v90_info1a_decode(
				frame->payload, sizeof(frame->payload),
				frame->payload_bit_count, &decoded.info1a) ==
			V90_INFO1_OK;
	else
		valid = v34_info1c_decode(
				frame->payload, sizeof(frame->payload),
				frame->payload_bit_count, &decoded.info1d) ==
			V34_INFO1_OK;
	if (!valid)
		return reject_peer_frame(exchange, event);
	if (phase2_info1_accept_peer(&exchange->control) != PHASE2_INFO1_OK)
		return false;
	exchange->peer_record = decoded;
	exchange->peer_record_received = true;
	event->peer_record = decoded;
	event->kind = exchange->control.signal_role == PHASE2_SIGNAL_B ?
			      PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED :
			      PCM_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED;
	return true;
}

static bool map_common_event(
	struct pcm_phase2_info1_exchange *exchange,
	const struct phase2_info1_event *common,
	struct pcm_phase2_info1_event *event)
{
	event->frame_event = common->frame_event;
	switch (common->kind) {
	case PHASE2_INFO1_EVENT_NONE:
		return true;
	case PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT:
		event->kind = PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT;
		return true;
	case PHASE2_INFO1_EVENT_PEER_FRAME_PENDING:
		return resolve_peer_frame(exchange, &common->frame_event, event);
	case PHASE2_INFO1_EVENT_LOCAL_SECOND_SENT:
		event->kind = PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT;
		return true;
	case PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED:
		event->kind = PCM_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
		return true;
	case PHASE2_INFO1_EVENT_FAILED:
		event->kind = PCM_PHASE2_INFO1_EVENT_FAILED;
		event->failure = common->failure;
		return true;
	}
	return false;
}

enum phase2_info1_result pcm_phase2_info1_sample(
	struct pcm_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_info1_event *event)
{
	struct pcm_phase2_info1_exchange next;
	struct pcm_phase2_info1_event emitted = { 0 };
	struct phase2_info1_event common;
	int16_t generated;

	if (!pcm_phase2_info1_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info1_sample(
		    &next.control, received_sample, &generated, &common) !=
		    PHASE2_INFO1_OK ||
	    !map_common_event(&next, &common, &emitted) ||
	    !pcm_phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INFO1_OK;
}
