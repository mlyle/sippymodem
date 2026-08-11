// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info1_exchange.h"

#include <stddef.h>

static bool role_is_valid(enum sippymodem_v34_station_role role)
{
	return role == SIPPYMODEM_V34_CALLING_STATION ||
	       role == SIPPYMODEM_V34_ANSWERING_STATION;
}

static enum sippymodem_v34_station_role probe_role(
	const struct sippymodem_v34_phase2_reciprocal_probe *probe)
{
	return probe->turnaround.initial_probe.ranging.info0_exchange.local_role;
}

static bool completed_probe_matches_role(
	const struct sippymodem_v34_phase2_reciprocal_probe *probe)
{
	return probe_role(probe) == SIPPYMODEM_V34_CALLING_STATION ?
		       probe->phase ==
			       SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_CALLER_COMPLETE :
		       probe->phase ==
			       SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A;
}

static int16_t probe_nominal_amplitude(
	const struct sippymodem_v34_phase2_reciprocal_probe *probe)
{
	const struct sippymodem_v34_phase2_tone_transmitter *tone =
		&probe->turnaround.initial_probe.ranging.info0_exchange
			 .tone_transmitter;

	return probe_role(probe) == SIPPYMODEM_V34_CALLING_STATION ?
		       tone->carrier_amplitude :
		       tone->guard_amplitude;
}

static uint32_t peer_deadline_samples(
	const struct sippymodem_v34_phase2_info1_exchange *exchange)
{
	return exchange->local_role == SIPPYMODEM_V34_CALLING_STATION ?
		       (uint32_t)
			       SIPPYMODEM_V34_PHASE2_CALLER_INFO1A_WAIT_BASE_SAMPLES +
			       exchange->round_trip_delay_samples :
		       (uint32_t)
			       SIPPYMODEM_V34_PHASE2_ANSWERER_INFO1C_WAIT_BASE_SAMPLES +
			       2u * exchange->round_trip_delay_samples;
}

static bool phase_has_peer_parameters(
	const struct sippymodem_v34_phase2_info1_exchange *exchange)
{
	if (exchange->local_role == SIPPYMODEM_V34_CALLING_STATION)
		return exchange->phase == SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE;
	return exchange->phase ==
		       SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED ||
	       exchange->phase == SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING ||
	       exchange->phase == SIPPYMODEM_V34_PHASE2_INFO1_CARRIER_TAIL ||
	       exchange->phase == SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE;
}

static bool peer_parameters_are_valid(
	const struct sippymodem_v34_phase2_info1_exchange *exchange)
{
	bool expected = phase_has_peer_parameters(exchange);

	if (exchange->peer_parameters_received != expected)
		return false;
	if (!expected)
		return true;
	return exchange->local_role == SIPPYMODEM_V34_CALLING_STATION ?
		       sippymodem_v34_info1a_is_valid(
			       &exchange->peer_parameters.info1a) :
		       sippymodem_v34_info1c_is_valid(
			       &exchange->peer_parameters.info1c);
}

static bool transmitter_matches_exchange(
	const struct sippymodem_v34_phase2_info1_exchange *exchange,
	bool frame_complete)
{
	const struct sippymodem_v34_phase2_info_audio_transmitter *transmitter =
		&exchange->transmitter;
	size_t payload_bits =
		exchange->local_role == SIPPYMODEM_V34_CALLING_STATION ?
			SIPPYMODEM_V34_INFO1C_PAYLOAD_BITS :
			SIPPYMODEM_V34_INFO1A_PAYLOAD_BITS;

	return sippymodem_v34_phase2_info_audio_transmitter_is_valid(
		       transmitter) &&
	       transmitter->modulation.local_role == exchange->local_role &&
	       transmitter->frame.payload_bit_count == payload_bits &&
	       transmitter->frame_complete == frame_complete;
}

static bool caller_state_is_valid(
	const struct sippymodem_v34_phase2_info1_exchange *exchange,
	uint32_t deadline)
{
	switch (exchange->phase) {
	case SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING:
		return exchange->peer_deadline_elapsed_samples == 0 &&
		       exchange->carrier_tail_symbols == 0 &&
		       transmitter_matches_exchange(exchange, false);
	case SIPPYMODEM_V34_PHASE2_INFO1_CARRIER_TAIL:
		return exchange->peer_deadline_elapsed_samples == 0 &&
		       exchange->carrier_tail_symbols <
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches_exchange(exchange, true);
	case SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols ==
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches_exchange(exchange, true);
	case SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols ==
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches_exchange(exchange, true);
	case SIPPYMODEM_V34_PHASE2_INFO1_FAILED:
		return exchange->peer_deadline_elapsed_samples == deadline &&
		       exchange->carrier_tail_symbols ==
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches_exchange(exchange, true);
	case SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED:
		return false;
	}
	return false;
}

static bool answerer_state_is_valid(
	const struct sippymodem_v34_phase2_info1_exchange *exchange,
	uint32_t deadline)
{
	switch (exchange->phase) {
	case SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols == 0;
	case SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols == 0;
	case SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols == 0 &&
		       transmitter_matches_exchange(exchange, false);
	case SIPPYMODEM_V34_PHASE2_INFO1_CARRIER_TAIL:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols <
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches_exchange(exchange, true);
	case SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE:
		return exchange->peer_deadline_elapsed_samples < deadline &&
		       exchange->carrier_tail_symbols ==
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches_exchange(exchange, true);
	case SIPPYMODEM_V34_PHASE2_INFO1_FAILED:
		return exchange->peer_deadline_elapsed_samples == deadline &&
		       exchange->carrier_tail_symbols == 0;
	}
	return false;
}

bool sippymodem_v34_phase2_info1_exchange_is_valid(
	const struct sippymodem_v34_phase2_info1_exchange *exchange)
{
	uint32_t deadline;
	bool failed;

	if (exchange == NULL || !role_is_valid(exchange->local_role) ||
	    exchange->nominal_amplitude <= 0 ||
	    (unsigned int)exchange->phase >
		    SIPPYMODEM_V34_PHASE2_INFO1_FAILED ||
	    (unsigned int)exchange->failure >
		    SIPPYMODEM_V34_PHASE2_INFO1_PEER_TIMEOUT ||
	    exchange->carrier_tail_symbols >
		    SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS ||
	    !sippymodem_v34_phase2_info_audio_receiver_is_valid(
		    &exchange->receiver) ||
	    exchange->receiver.modulation.local_role != exchange->local_role ||
	    exchange->receiver.frame.expected_payload_bit_count !=
		    (exchange->local_role ==
			     SIPPYMODEM_V34_CALLING_STATION ?
			     SIPPYMODEM_V34_INFO1A_PAYLOAD_BITS :
			     SIPPYMODEM_V34_INFO1C_PAYLOAD_BITS) ||
	    !peer_parameters_are_valid(exchange))
		return false;
	deadline = peer_deadline_samples(exchange);
	if (exchange->peer_deadline_elapsed_samples > deadline)
		return false;
	failed = exchange->phase == SIPPYMODEM_V34_PHASE2_INFO1_FAILED;
	if (failed !=
	    (exchange->failure == SIPPYMODEM_V34_PHASE2_INFO1_PEER_TIMEOUT))
		return false;
	if (exchange->local_role ==
	    SIPPYMODEM_V34_CALLING_STATION)
		return caller_state_is_valid(exchange, deadline);
	return sippymodem_v34_phase2_tone_transmitter_is_valid(
		       &exchange->answerer_tone_a) &&
	       exchange->answerer_tone_a.local_role ==
		       SIPPYMODEM_V34_ANSWERING_STATION &&
	       exchange->answerer_tone_a.guard_amplitude ==
		       exchange->nominal_amplitude &&
	       answerer_state_is_valid(exchange, deadline);
}

static void initialize_handoff_context(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_phase2_reciprocal_probe *probe)
{
	exchange->local_role = probe_role(probe);
	exchange->round_trip_delay_samples =
		probe->turnaround.initial_probe.ranging.round_trip_delay_samples;
	exchange->nominal_amplitude = probe_nominal_amplitude(probe);
}

enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_caller_init(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_phase2_reciprocal_probe *completed_probe,
	const struct sippymodem_v34_info1c *local_info1c)
{
	struct sippymodem_v34_phase2_info1_exchange initialized = {
		.phase = SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING,
	};
	uint8_t payload[SIPPYMODEM_V34_INFO1C_PAYLOAD_BYTES];

	if (exchange == NULL ||
	    !sippymodem_v34_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) !=
		    SIPPYMODEM_V34_CALLING_STATION ||
	    sippymodem_v34_info1c_encode(
		    local_info1c, payload, sizeof(payload)) !=
		    SIPPYMODEM_V34_INFO1_OK)
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	initialize_handoff_context(&initialized, completed_probe);
	if (sippymodem_v34_phase2_info_audio_transmitter_init(
		    &initialized.transmitter, initialized.local_role,
		    initialized.nominal_amplitude, payload,
		    SIPPYMODEM_V34_INFO1C_PAYLOAD_BITS) !=
		    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK ||
	    sippymodem_v34_phase2_info_audio_receiver_init(
		    &initialized.receiver, initialized.local_role,
		    SIPPYMODEM_V34_INFO1A_PAYLOAD_BITS) !=
		    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK ||
	    !sippymodem_v34_phase2_info1_exchange_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK;
}

enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_answerer_init(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_phase2_reciprocal_probe *completed_probe)
{
	struct sippymodem_v34_phase2_info1_exchange initialized = {
		.phase = SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER,
	};

	if (exchange == NULL ||
	    !sippymodem_v34_phase2_reciprocal_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe) ||
	    probe_role(completed_probe) !=
		    SIPPYMODEM_V34_ANSWERING_STATION)
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	initialize_handoff_context(&initialized, completed_probe);
	initialized.answerer_tone_a = completed_probe->turnaround.initial_probe
					      .ranging.info0_exchange
					      .tone_transmitter;
	initialized.peer_deadline_elapsed_samples =
		completed_probe->answerer_info1c_elapsed_samples;
	if (initialized.peer_deadline_elapsed_samples >=
		    peer_deadline_samples(&initialized) ||
	    sippymodem_v34_phase2_info_audio_receiver_init(
		    &initialized.receiver, initialized.local_role,
		    SIPPYMODEM_V34_INFO1C_PAYLOAD_BITS) !=
		    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK ||
	    !sippymodem_v34_phase2_info1_exchange_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK;
}

static void align_answerer_info_with_tone(
	struct sippymodem_v34_phase2_info1_exchange *exchange)
{
	struct sippymodem_v34_phase2_dpsk_transmitter *info =
		&exchange->transmitter.modulation;

	/* The tone oscillator uses sine and the QAM carrier uses cosine. Both
	 * phase fields identify the following sample at this boundary. */
	info->carrier.carrier_phase =
		exchange->answerer_tone_a.carrier_phase - UINT32_C(0x40000000);
	info->guard_phase = exchange->answerer_tone_a.guard_phase;
}

enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_answerer_reply(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_info1a *local_info1a)
{
	struct sippymodem_v34_phase2_info1_exchange next;
	uint8_t payload[SIPPYMODEM_V34_INFO1A_PAYLOAD_BYTES];

	if (!sippymodem_v34_phase2_info1_exchange_is_valid(exchange) ||
	    exchange->phase !=
		    SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED ||
	    sippymodem_v34_info1a_encode(
		    local_info1a, payload, sizeof(payload)) !=
		    SIPPYMODEM_V34_INFO1_OK)
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (sippymodem_v34_phase2_info_audio_transmitter_init(
		    &next.transmitter, next.local_role,
		    next.nominal_amplitude, payload,
		    SIPPYMODEM_V34_INFO1A_PAYLOAD_BITS) !=
	    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK)
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	align_answerer_info_with_tone(&next);
	next.phase = SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING;
	if (!sippymodem_v34_phase2_info1_exchange_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK;
}

static void reject_peer_frame(
	struct sippymodem_v34_phase2_info1_exchange_event *event)
{
	event->kind = SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
}

static bool receive_peer_parameters(
	struct sippymodem_v34_phase2_info1_exchange *exchange, int16_t sample,
	struct sippymodem_v34_phase2_info1_exchange_event *event)
{
	struct sippymodem_v34_phase2_info_event frame_event;
	union sippymodem_v34_phase2_info1_parameters decoded = { 0 };

	if (sippymodem_v34_phase2_info_audio_receiver_sample(
		    &exchange->receiver, sample, &frame_event) !=
	    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK)
		return false;
	event->frame_event = frame_event;
	if (frame_event.type == SIPPYMODEM_V34_PHASE2_INFO_FRAME_REJECTED) {
		reject_peer_frame(event);
		return true;
	}
	if (frame_event.type != SIPPYMODEM_V34_PHASE2_INFO_FRAME_RECEIVED)
		return true;
	if (exchange->local_role == SIPPYMODEM_V34_CALLING_STATION) {
		if (sippymodem_v34_info1a_decode(
			    frame_event.payload, sizeof(frame_event.payload),
			    frame_event.payload_bit_count, &decoded.info1a) !=
		    SIPPYMODEM_V34_INFO1_OK) {
			reject_peer_frame(event);
			return true;
		}
		exchange->phase = SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE;
		event->kind =
			SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED;
	} else {
		if (sippymodem_v34_info1c_decode(
			    frame_event.payload, sizeof(frame_event.payload),
			    frame_event.payload_bit_count, &decoded.info1c) !=
		    SIPPYMODEM_V34_INFO1_OK) {
			reject_peer_frame(event);
			return true;
		}
		exchange->phase =
			SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED;
		event->kind =
			SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED;
	}
	exchange->peer_parameters = decoded;
	exchange->peer_parameters_received = true;
	event->peer_parameters = decoded;
	return true;
}

static bool generate_answerer_tone(
	struct sippymodem_v34_phase2_info1_exchange *exchange, int16_t *sample)
{
	return sippymodem_v34_phase2_tone_transmitter_sample(
		       &exchange->answerer_tone_a, sample) ==
	       SIPPYMODEM_V34_PHASE2_TONE_OK;
}

static bool generate_sequence(
	struct sippymodem_v34_phase2_info1_exchange *exchange, int16_t *sample)
{
	bool frame_complete;

	if (sippymodem_v34_phase2_info_audio_transmitter_sample(
		    &exchange->transmitter, sample, &frame_complete) !=
	    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK)
		return false;
	if (frame_complete)
		exchange->phase = SIPPYMODEM_V34_PHASE2_INFO1_CARRIER_TAIL;
	return true;
}

static bool generate_carrier_tail(
	struct sippymodem_v34_phase2_info1_exchange *exchange, int16_t *sample,
	struct sippymodem_v34_phase2_info1_exchange_event *event)
{
	bool bit_accepted;

	if (sippymodem_v34_phase2_dpsk_transmitter_sample(
		    &exchange->transmitter.modulation, 0, sample,
		    &bit_accepted) != SIPPYMODEM_V34_PHASE2_DPSK_OK)
		return false;
	if (!bit_accepted)
		return true;
	if (++exchange->carrier_tail_symbols <
	    SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS)
		return true;
	if (exchange->local_role == SIPPYMODEM_V34_CALLING_STATION) {
		exchange->phase = SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER;
		event->kind =
			SIPPYMODEM_V34_PHASE2_INFO1_EVENT_LOCAL_INFO1C_SENT;
	} else {
		exchange->phase = SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE;
		event->kind =
			SIPPYMODEM_V34_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT;
	}
	return true;
}

static bool generate_transmit_sample(
	struct sippymodem_v34_phase2_info1_exchange *exchange, int16_t *sample,
	struct sippymodem_v34_phase2_info1_exchange_event *event)
{
	switch (exchange->phase) {
	case SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING:
		return generate_sequence(exchange, sample);
	case SIPPYMODEM_V34_PHASE2_INFO1_CARRIER_TAIL:
		return generate_carrier_tail(exchange, sample, event);
	case SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER:
	case SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED:
		if (exchange->local_role ==
		    SIPPYMODEM_V34_ANSWERING_STATION)
			return generate_answerer_tone(exchange, sample);
		*sample = 0;
		return true;
	case SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE:
	case SIPPYMODEM_V34_PHASE2_INFO1_FAILED:
		*sample = 0;
		return true;
	}
	return false;
}

static void advance_peer_deadline(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	struct sippymodem_v34_phase2_info1_exchange_event *event)
{
	if (++exchange->peer_deadline_elapsed_samples <
	    peer_deadline_samples(exchange))
		return;
	exchange->phase = SIPPYMODEM_V34_PHASE2_INFO1_FAILED;
	exchange->failure = SIPPYMODEM_V34_PHASE2_INFO1_PEER_TIMEOUT;
	event->kind = SIPPYMODEM_V34_PHASE2_INFO1_EVENT_FAILED;
	event->failure = exchange->failure;
}

enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_sample(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_info1_exchange_event *event)
{
	struct sippymodem_v34_phase2_info1_exchange next;
	struct sippymodem_v34_phase2_info1_exchange_event emitted = { 0 };
	int16_t generated = 0;
	bool waited_for_peer;

	if (!sippymodem_v34_phase2_info1_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	waited_for_peer =
		next.phase == SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER;
	if ((waited_for_peer &&
	     !receive_peer_parameters(&next, received_sample, &emitted)) ||
	    !generate_transmit_sample(&next, &generated, &emitted))
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	if (waited_for_peer &&
	    next.phase == SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER)
		advance_peer_deadline(&next, &emitted);
	if (!sippymodem_v34_phase2_info1_exchange_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK;
}

const char *sippymodem_v34_phase2_info1_exchange_result_name(
	enum sippymodem_v34_phase2_info1_exchange_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
