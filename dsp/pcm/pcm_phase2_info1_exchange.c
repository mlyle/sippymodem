// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_info1_exchange.h"

#include <stddef.h>

_Static_assert((int)V92_INFO1D_PAYLOAD_BITS == (int)V34_INFO1C_PAYLOAD_BITS,
	"PCM INFO1d records must share one transport width");
_Static_assert((int)V90_INFO1A_PAYLOAD_BITS == (int)V34_INFO1A_PAYLOAD_BITS,
	"V.90 and V.34 INFO1a records must share one transport width");
_Static_assert(
	(int)V92_PCM_INFO1A_PAYLOAD_BITS == (int)V90_INFO1A_PAYLOAD_BITS,
	"PCM INFO1a records must share one transport width");

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
		return control->phase == PHASE2_INFO1_SILENCE ||
		       control->phase == PHASE2_INFO1_COMPLETE;
	return control->phase == PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED ||
	       control->phase == PHASE2_INFO1_TRANSMITTING ||
	       control->phase == PHASE2_INFO1_CARRIER_TAIL ||
	       control->phase == PHASE2_INFO1_SILENCE ||
	       control->phase == PHASE2_INFO1_COMPLETE;
}

static bool phase_has_local_analog_record(
	const struct pcm_phase2_info1_exchange *exchange)
{
	if (exchange->control.signal_role != PHASE2_SIGNAL_A)
		return false;
	return exchange->control.phase == PHASE2_INFO1_TRANSMITTING ||
	       exchange->control.phase == PHASE2_INFO1_CARRIER_TAIL ||
	       exchange->control.phase == PHASE2_INFO1_SILENCE ||
	       exchange->control.phase == PHASE2_INFO1_COMPLETE;
}

static bool analog_record_is_valid(
	const struct pcm_phase2_info1_exchange *exchange)
{
	if (exchange->control.short_exchange) {
		struct pcm_info1_selection selected = {
			.startup = { exchange->protocol, PCM_PHASE2_SHORT },
			.short_high_carrier = exchange->analog.short_high_carrier,
			.analog = exchange->analog.record,
			.upstream = exchange->analog.kind == PCM_INFO1A_PCM ?
				PCM_UPSTREAM_PCM : PCM_UPSTREAM_V34,
		};

		return (exchange->analog.kind == PCM_INFO1A_PCM ||
			exchange->analog.kind == PCM_INFO1A_CONVENTIONAL) &&
		       pcm_info1_selection_is_valid(&selected);
	}
	if (exchange->analog.short_high_carrier)
		return false;
	switch (exchange->analog.kind) {
	case PCM_INFO1A_CONVENTIONAL:
		return v90_info1a_is_valid(
			&exchange->analog.record.conventional);
	case PCM_INFO1A_PCM:
		return exchange->protocol == PCM_PROTOCOL_V92 &&
		       exchange->digital_record_stored &&
		       exchange->digital.protocol == PCM_PROTOCOL_V92 &&
		       exchange->digital.record.v92.pcm_upstream_supported &&
		       v92_pcm_info1a_is_valid(&exchange->analog.record.pcm);
	case PCM_INFO1A_V34_FALLBACK:
		return v34_info1a_is_valid(&exchange->analog.record.v34_fallback);
	}
	return false;
}

bool pcm_phase2_info1_is_valid(
	const struct pcm_phase2_info1_exchange *exchange)
{
	bool peer_record;
	bool expect_digital;
	bool expect_analog;

	if (exchange == NULL ||
	    !pcm_protocol_is_valid(exchange->protocol) ||
	    !phase2_info1_is_valid(&exchange->control) ||
	    exchange->info1d_resends > 1u ||
	    (exchange->info1d_resends != 0u &&
	     exchange->control.signal_role != PHASE2_SIGNAL_B) ||
	    exchange->control.phase == PHASE2_INFO1_PEER_FRAME_PENDING ||
	    exchange->control.first_payload_bit_count !=
		    (exchange->control.short_exchange ? 0u : V34_INFO1C_PAYLOAD_BITS) ||
	    exchange->control.second_payload_bit_count !=
		    V90_INFO1A_PAYLOAD_BITS)
		return false;
	if (exchange->control.short_exchange &&
	    (exchange->protocol != PCM_PROTOCOL_V92 ||
	     exchange->info1d_resends != 0u))
		return false;
	peer_record = phase_has_peer_record(exchange);
	expect_digital = !exchange->control.short_exchange &&
		(exchange->control.signal_role == PHASE2_SIGNAL_B || peer_record);
	expect_analog = exchange->control.signal_role == PHASE2_SIGNAL_A ?
			phase_has_local_analog_record(exchange) : peer_record;
	if (exchange->digital_record_stored != expect_digital ||
	    exchange->analog_record_stored != expect_analog)
		return false;
	if (expect_digital &&
	    (!pcm_info1d_is_valid(&exchange->digital) ||
	     exchange->digital.protocol != exchange->protocol))
		return false;
	return !expect_analog || analog_record_is_valid(exchange);
}

enum phase2_info1_result pcm_phase2_info1_init_short_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v34_phase2_tone_transmitter *tone,
	const struct pcm_info1_selection *selection)
{
	struct pcm_phase2_info1_exchange initialized = {
		.protocol = PCM_PROTOCOL_V92,
	};
	uint8_t payload[V90_INFO1A_PAYLOAD_BYTES];

	if (exchange == NULL ||
	    pcm_info1_encode_short(selection, payload, sizeof(payload)) !=
		    PCM_INFO1_OK ||
	    phase2_info1_init_short_signal_a(&initialized.control, tone, payload,
		    V90_INFO1A_PAYLOAD_BITS) != PHASE2_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	initialized.analog = (struct pcm_phase2_info1a){
		.record = selection->analog,
		.kind = selection->upstream == PCM_UPSTREAM_PCM ?
			PCM_INFO1A_PCM : PCM_INFO1A_CONVENTIONAL,
		.short_high_carrier = selection->short_high_carrier,
	};
	initialized.analog_record_stored = true;
	if (!pcm_phase2_info1_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result pcm_phase2_info1_init_short_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct pcm_startup_selection *startup, uint16_t remaining_samples)
{
	struct pcm_phase2_info1_exchange initialized = {
		.protocol = PCM_PROTOCOL_V92,
	};

	if (exchange == NULL || !pcm_startup_selection_is_valid(startup) ||
	    startup->phase2 != PCM_PHASE2_SHORT ||
	    phase2_info1_init_short_signal_b(&initialized.control, remaining_samples,
		    V90_INFO1A_PAYLOAD_BITS) != PHASE2_INFO1_OK ||
	    !pcm_phase2_info1_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

bool pcm_phase2_info1_retry_pending(
	const struct pcm_phase2_info1_exchange *exchange)
{
	return pcm_phase2_info1_is_valid(exchange) &&
	       exchange->control.signal_role == PHASE2_SIGNAL_B &&
	       exchange->control.phase == PHASE2_INFO1_RECOVERING &&
	       exchange->info1d_resends == 0u;
}

static enum phase2_info1_result initialize_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct pcm_info1d *local_info1d, const uint8_t *payload)
{
	struct pcm_phase2_info1_exchange initialized = { 0 };

	if (exchange == NULL || payload == NULL ||
	    !pcm_info1d_is_valid(local_info1d) ||
	    !v90_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) != V90_DIGITAL_MODEM)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	initialized.digital = *local_info1d;
	initialized.protocol = local_info1d->protocol;
	initialized.digital_record_stored = true;
	if (phase2_info1_init_signal_b(
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

enum phase2_info1_result pcm_phase2_info1_init_v90_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct v34_info1c *local_info1d)
{
	struct pcm_info1d tagged;
	uint8_t payload[V34_INFO1C_PAYLOAD_BYTES];

	if (pcm_info1d_init_v90(&tagged, local_info1d) != PCM_INFO1_OK ||
	    v34_info1c_encode(local_info1d, payload, sizeof(payload)) !=
		    V34_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	return initialize_digital(
		exchange, completed_probe, &tagged, payload);
}

enum phase2_info1_result pcm_phase2_info1_init_v92_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct v92_info1d *local_info1d)
{
	struct pcm_info1d tagged;
	uint8_t payload[V92_INFO1D_PAYLOAD_BYTES];

	if (pcm_info1d_init_v92(&tagged, local_info1d) != PCM_INFO1_OK ||
	    v92_info1d_encode(local_info1d, payload, sizeof(payload)) !=
		    V92_INFO1D_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	return initialize_digital(
		exchange, completed_probe, &tagged, payload);
}

static enum phase2_info1_result initialize_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	enum pcm_protocol protocol)
{
	struct pcm_phase2_info1_exchange initialized = {
		.protocol = protocol,
	};

	if (exchange == NULL || !pcm_protocol_is_valid(protocol) ||
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

enum phase2_info1_result pcm_phase2_info1_init_v90_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe)
{
	return initialize_analog(exchange, completed_probe, PCM_PROTOCOL_V90);
}

enum phase2_info1_result pcm_phase2_info1_init_v92_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe)
{
	return initialize_analog(exchange, completed_probe, PCM_PROTOCOL_V92);
}

static enum phase2_info1_result reply_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct pcm_phase2_info1a *local_info1a,
	const uint8_t *payload)
{
	struct pcm_phase2_info1_exchange next;

	if (!pcm_phase2_info1_is_valid(exchange) || local_info1a == NULL ||
	    payload == NULL || exchange->control.signal_role != PHASE2_SIGNAL_A)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (phase2_info1_reply_signal_a(
		    &next.control, payload, V90_INFO1A_PAYLOAD_BITS) !=
		    PHASE2_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next.analog = *local_info1a;
	next.analog_record_stored = true;
	if (!pcm_phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result pcm_phase2_info1_reply_conventional(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_info1a *local_info1a)
{
	struct pcm_phase2_info1a selected = {
		.kind = PCM_INFO1A_CONVENTIONAL,
	};
	uint8_t payload[V90_INFO1A_PAYLOAD_BYTES];

	if (v90_info1a_encode(local_info1a, payload, sizeof(payload)) !=
	    V90_INFO1_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	selected.record.conventional = *local_info1a;
	return reply_analog(exchange, &selected, payload);
}

enum phase2_info1_result pcm_phase2_info1_reply_pcm(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v92_pcm_info1a *local_info1a)
{
	struct pcm_phase2_info1a selected = {
		.kind = PCM_INFO1A_PCM,
	};
	uint8_t payload[V92_PCM_INFO1A_PAYLOAD_BYTES];

	if (!pcm_phase2_info1_is_valid(exchange) ||
	    exchange->protocol != PCM_PROTOCOL_V92 ||
	    !exchange->digital_record_stored ||
	    !exchange->digital.record.v92.pcm_upstream_supported ||
	    v92_pcm_info1a_encode(local_info1a, payload, sizeof(payload)) !=
		    V92_PCM_INFO1A_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	selected.record.pcm = *local_info1a;
	return reply_analog(exchange, &selected, payload);
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

static bool decode_digital_record(
	const struct pcm_phase2_info1_exchange *exchange,
	const struct v34_phase2_info_event *frame,
	struct pcm_info1d *decoded)
{
	if (exchange->protocol == PCM_PROTOCOL_V90) {
		struct v34_info1c record;

		return v34_info1c_decode(
			       frame->payload, sizeof(frame->payload),
			       frame->payload_bit_count, &record) == V34_INFO1_OK &&
		       pcm_info1d_init_v90(decoded, &record) == PCM_INFO1_OK;
	}
	{
		struct v92_info1d record;

		return v92_info1d_decode(
			       frame->payload, sizeof(frame->payload),
			       frame->payload_bit_count, &record) == V92_INFO1D_OK &&
		       pcm_info1d_init_v92(decoded, &record) == PCM_INFO1_OK;
	}
}

static bool decode_analog_record(
	const struct pcm_phase2_info1_exchange *exchange,
	const struct v34_phase2_info_event *frame,
	struct pcm_phase2_info1a *decoded)
{
	struct v90_info1a conventional;
	struct v34_info1a fallback;
	struct v92_pcm_info1a pcm;
	bool conventional_valid;
	bool fallback_valid;
	bool pcm_valid;

	if (exchange->control.short_exchange) {
		const struct pcm_startup_selection startup = {
			exchange->protocol, PCM_PHASE2_SHORT,
		};
		struct pcm_info1_selection selected;

		if (pcm_info1_select_short(&startup, frame->payload,
			    sizeof(frame->payload),
			    frame->payload_bit_count, &selected) != PCM_INFO1_OK)
			return false;
		*decoded = (struct pcm_phase2_info1a){
			.record = selected.analog,
			.kind = selected.upstream == PCM_UPSTREAM_PCM ?
				PCM_INFO1A_PCM : PCM_INFO1A_CONVENTIONAL,
			.short_high_carrier = selected.short_high_carrier,
		};
		return true;
	}

	conventional_valid = v90_info1a_decode(
		frame->payload, sizeof(frame->payload),
		frame->payload_bit_count, &conventional) == V90_INFO1_OK;
	fallback_valid = v34_info1a_decode(
		frame->payload, sizeof(frame->payload),
		frame->payload_bit_count, &fallback) == V34_INFO1_OK;
	pcm_valid = exchange->protocol == PCM_PROTOCOL_V92 &&
		v92_pcm_info1a_decode(
			frame->payload, sizeof(frame->payload),
			frame->payload_bit_count, &pcm) == V92_PCM_INFO1A_OK;
	if ((unsigned int)conventional_valid + fallback_valid + pcm_valid != 1u)
		return false;
	if (conventional_valid) {
		*decoded = (struct pcm_phase2_info1a){
			.record.conventional = conventional,
			.kind = PCM_INFO1A_CONVENTIONAL,
		};
		return true;
	}
	if (fallback_valid) {
		*decoded = (struct pcm_phase2_info1a){
			.record.v34_fallback = fallback,
			.kind = PCM_INFO1A_V34_FALLBACK,
		};
		return true;
	}
	if (!exchange->digital.record.v92.pcm_upstream_supported)
		return false;
	*decoded = (struct pcm_phase2_info1a){
		.record.pcm = pcm,
		.kind = PCM_INFO1A_PCM,
	};
	return true;
}

static bool resolve_peer_frame(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v34_phase2_info_event *frame,
	struct pcm_phase2_info1_event *event)
{
	struct pcm_info1d digital = { 0 };
	struct pcm_phase2_info1a analog = { 0 };
	bool peer_is_analog =
		exchange->control.signal_role == PHASE2_SIGNAL_B;
	bool valid = peer_is_analog ?
		decode_analog_record(exchange, frame, &analog) :
		decode_digital_record(exchange, frame, &digital);

	if (!valid)
		return reject_peer_frame(exchange, event);
	if (phase2_info1_accept_peer(&exchange->control) != PHASE2_INFO1_OK)
		return false;
	if (peer_is_analog) {
		exchange->analog = analog;
		exchange->analog_record_stored = true;
		event->peer_record.analog = analog;
		event->kind = PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED;
	} else {
		exchange->digital = digital;
		exchange->digital_record_stored = true;
		event->peer_record.digital = digital;
		event->kind = PCM_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED;
	}
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
	case PHASE2_INFO1_EVENT_PEER_FRAME_STARTED:
		return true;
	case PHASE2_INFO1_EVENT_PEER_INFOMARKS:
		/* V.92 9.3 shares V.90 9.2.1.2.6 recovery: INFOMARKSa
		 * permits INFO1d resend or full retrain. Bound resends to one
		 * before selecting full retrain on a repeated request. */
		if (exchange->control.signal_role != PHASE2_SIGNAL_B ||
		    exchange->control.phase != PHASE2_INFO1_RECOVERING)
			return true;
		if (!pcm_phase2_info1_retry_pending(exchange))
			event->kind =
				PCM_PHASE2_INFO1_EVENT_RETRAIN_REQUESTED;
		else {
			if (phase2_info1_repeat_signal_b(&exchange->control) !=
			    PHASE2_INFO1_OK)
				return false;
			exchange->info1d_resends++;
		}
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
	emitted.protocol = next.protocol;
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
