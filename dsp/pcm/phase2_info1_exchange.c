// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_info1_exchange.h"

#include <stddef.h>
#include <string.h>

_Static_assert(PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES == 560,
	"INFO1a silence must follow the exact 8 kHz sample clock");

static uint32_t peer_deadline_samples(
	const struct phase2_info1_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B ?
		       (uint32_t)PHASE2_INFO1_SIGNAL_B_WAIT_BASE_SAMPLES +
			       exchange->round_trip_delay_samples :
		       (uint32_t)PHASE2_INFO1_SIGNAL_A_WAIT_BASE_SAMPLES +
			       2u * exchange->round_trip_delay_samples;
}

static size_t local_payload_bits(
	const struct phase2_info1_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B ?
		       exchange->first_payload_bit_count :
		       exchange->second_payload_bit_count;
}

static size_t peer_payload_bits(
	const struct phase2_info1_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B ?
		       exchange->second_payload_bit_count :
		       exchange->first_payload_bit_count;
}

static bool transmitter_matches(
	const struct phase2_info1_exchange *exchange, bool frame_complete)
{
	const struct v34_phase2_info_audio_transmitter *transmitter =
		&exchange->transmitter;

	return v34_phase2_info_audio_transmitter_is_valid(transmitter) &&
	       transmitter->modulation.local_role ==
		       v34_phase2_station_for_signal(exchange->signal_role) &&
	       transmitter->frame.payload_bit_count ==
		       local_payload_bits(exchange) &&
	       transmitter->frame_complete == frame_complete;
}

static bool silence_state_is_valid(
	const struct phase2_info1_exchange *exchange)
{
	if (exchange->phase == PHASE2_INFO1_SILENCE)
		return exchange->silence_samples <
		       PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES;
	if (exchange->phase == PHASE2_INFO1_COMPLETE)
		return exchange->silence_samples ==
		       PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES;
	return exchange->silence_samples == 0;
}

static bool signal_b_state_is_valid(
	const struct phase2_info1_exchange *exchange, uint32_t deadline)
{
	switch (exchange->phase) {
	case PHASE2_INFO1_TRANSMITTING:
		return exchange->peer_deadline_elapsed_samples == 0 &&
		       exchange->carrier_tail_symbols == 0 &&
		       transmitter_matches(exchange, false);
	case PHASE2_INFO1_CARRIER_TAIL:
		return exchange->peer_deadline_elapsed_samples == 0 &&
		       exchange->carrier_tail_symbols <
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_WAITING_FOR_PEER:
	case PHASE2_INFO1_PEER_FRAME_PENDING:
		return exchange->peer_deadline_elapsed_samples <= deadline &&
		       exchange->carrier_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_RECOVERING:
		return exchange->peer_deadline_elapsed_samples == deadline &&
		       exchange->carrier_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_SILENCE:
	case PHASE2_INFO1_COMPLETE:
		return exchange->peer_deadline_elapsed_samples <= deadline &&
		       exchange->carrier_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_FAILED:
		return exchange->peer_deadline_elapsed_samples == deadline &&
		       exchange->carrier_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED:
		return false;
	}
	return false;
}

static bool signal_a_state_is_valid(
	const struct phase2_info1_exchange *exchange, uint32_t deadline)
{
	switch (exchange->phase) {
	case PHASE2_INFO1_WAITING_FOR_PEER:
	case PHASE2_INFO1_PEER_FRAME_PENDING:
	case PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED:
		return exchange->peer_deadline_elapsed_samples <= deadline &&
		       exchange->carrier_tail_symbols == 0;
	case PHASE2_INFO1_RECOVERING:
		return exchange->peer_deadline_elapsed_samples == deadline &&
		       exchange->carrier_tail_symbols == 0;
	case PHASE2_INFO1_TRANSMITTING:
		return exchange->peer_deadline_elapsed_samples <= deadline &&
		       exchange->carrier_tail_symbols == 0 &&
		       transmitter_matches(exchange, false);
	case PHASE2_INFO1_CARRIER_TAIL:
		return exchange->peer_deadline_elapsed_samples <= deadline &&
		       exchange->carrier_tail_symbols <
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_SILENCE:
	case PHASE2_INFO1_COMPLETE:
		return exchange->peer_deadline_elapsed_samples <= deadline &&
		       exchange->carrier_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       transmitter_matches(exchange, true);
	case PHASE2_INFO1_FAILED:
		return exchange->peer_deadline_elapsed_samples == deadline &&
		       exchange->carrier_tail_symbols == 0;
	}
	return false;
}

bool phase2_info1_is_valid(const struct phase2_info1_exchange *exchange)
{
	uint32_t deadline;
	bool failed;

	if (exchange == NULL ||
	    !phase2_signal_role_is_valid(exchange->signal_role) ||
	    exchange->nominal_amplitude <= 0 ||
	    exchange->first_payload_bit_count >
		    V34_PHASE2_INFO_MAX_PAYLOAD_BITS ||
	    exchange->second_payload_bit_count >
		    V34_PHASE2_INFO_MAX_PAYLOAD_BITS ||
	    (unsigned int)exchange->phase > PHASE2_INFO1_FAILED ||
	    !silence_state_is_valid(exchange) ||
	    (unsigned int)exchange->failure > PHASE2_INFO1_PEER_TIMEOUT ||
	    exchange->carrier_tail_symbols >
		    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS ||
	    !v34_phase2_info_audio_receiver_is_valid(&exchange->receiver) ||
	    exchange->receiver.modulation.local_role !=
		    v34_phase2_station_for_signal(exchange->signal_role) ||
	    exchange->receiver.frame.expected_payload_bit_count !=
		    peer_payload_bits(exchange))
		return false;
	deadline = peer_deadline_samples(exchange);
	if (exchange->peer_deadline_elapsed_samples > deadline)
		return false;
	failed = exchange->phase == PHASE2_INFO1_FAILED;
	if (failed != (exchange->failure == PHASE2_INFO1_PEER_TIMEOUT))
		return false;
	if (exchange->signal_role == PHASE2_SIGNAL_B)
		return signal_b_state_is_valid(exchange, deadline);
	return v34_phase2_tone_transmitter_is_valid(
		       &exchange->signal_a_tone) &&
	       exchange->signal_a_tone.local_role ==
		       V34_ANSWERING_STATION &&
	       exchange->signal_a_tone.guard_amplitude ==
		       exchange->nominal_amplitude &&
	       signal_a_state_is_valid(exchange, deadline);
}

bool phase2_info1_is_waiting_for_peer(
	const struct phase2_info1_exchange *exchange)
{
	return exchange != NULL &&
	       (exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER ||
		exchange->phase == PHASE2_INFO1_RECOVERING);
}

enum phase2_info1_result phase2_info1_init_signal_b(
	struct phase2_info1_exchange *exchange,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples,
	const uint8_t *first_payload, size_t first_payload_bit_count,
	size_t second_payload_bit_count)
{
	struct phase2_info1_exchange initialized = {
		.round_trip_delay_samples = round_trip_delay_samples,
		.first_payload_bit_count = first_payload_bit_count,
		.second_payload_bit_count = second_payload_bit_count,
		.nominal_amplitude = nominal_amplitude,
		.signal_role = PHASE2_SIGNAL_B,
		.phase = PHASE2_INFO1_TRANSMITTING,
	};

	if (exchange == NULL ||
	    v34_phase2_info_audio_transmitter_init(
		    &initialized.transmitter, V34_CALLING_STATION,
		    nominal_amplitude, first_payload,
		    first_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK ||
	    v34_phase2_info_audio_receiver_init(
		    &initialized.receiver, V34_CALLING_STATION,
		    second_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK ||
	    !phase2_info1_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result phase2_info1_init_signal_a(
	struct phase2_info1_exchange *exchange,
	const struct v34_phase2_tone_transmitter *signal_a_tone,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples,
	uint32_t elapsed_samples, size_t first_payload_bit_count,
	size_t second_payload_bit_count)
{
	struct phase2_info1_exchange initialized = {
		.peer_deadline_elapsed_samples = elapsed_samples,
		.round_trip_delay_samples = round_trip_delay_samples,
		.first_payload_bit_count = first_payload_bit_count,
		.second_payload_bit_count = second_payload_bit_count,
		.nominal_amplitude = nominal_amplitude,
		.signal_role = PHASE2_SIGNAL_A,
		.phase = PHASE2_INFO1_WAITING_FOR_PEER,
	};

	if (exchange == NULL || signal_a_tone == NULL)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	initialized.signal_a_tone = *signal_a_tone;
	if (elapsed_samples >= peer_deadline_samples(&initialized) ||
	    v34_phase2_info_audio_receiver_init(
		    &initialized.receiver, V34_ANSWERING_STATION,
		    first_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK ||
	    !phase2_info1_is_valid(&initialized))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO1_OK;
}

static void advance_peer_deadline(struct phase2_info1_exchange *exchange)
{
	if (++exchange->peer_deadline_elapsed_samples <
	    peer_deadline_samples(exchange))
		return;
	exchange->phase = PHASE2_INFO1_FAILED;
	exchange->failure = PHASE2_INFO1_PEER_TIMEOUT;
}

enum phase2_info1_result phase2_info1_accept_peer(
	struct phase2_info1_exchange *exchange)
{
	struct phase2_info1_exchange next;

	if (!phase2_info1_is_valid(exchange) ||
	    exchange->phase != PHASE2_INFO1_PEER_FRAME_PENDING)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (next.signal_role == PHASE2_SIGNAL_B) {
		next.silence_samples =
			v34_phase2_info_audio_receiver_handoff_delay_samples(
				&next.receiver);
		if (next.silence_samples >=
		    PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES)
			return PHASE2_INFO1_INVALID_ARGUMENT;
		next.phase = PHASE2_INFO1_SILENCE;
	} else {
		next.phase = PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED;
	}
	if (!phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result phase2_info1_reject_peer(
	struct phase2_info1_exchange *exchange)
{
	struct phase2_info1_exchange next;

	if (!phase2_info1_is_valid(exchange) ||
	    exchange->phase != PHASE2_INFO1_PEER_FRAME_PENDING)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	next.phase = PHASE2_INFO1_WAITING_FOR_PEER;
	advance_peer_deadline(&next);
	if (!phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result phase2_info1_recover(
	struct phase2_info1_exchange *exchange)
{
	struct phase2_info1_exchange next;

	if (!phase2_info1_is_valid(exchange) ||
	    exchange->phase != PHASE2_INFO1_FAILED ||
	    exchange->failure != PHASE2_INFO1_PEER_TIMEOUT)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	next.phase = PHASE2_INFO1_RECOVERING;
	next.failure = PHASE2_INFO1_NO_FAILURE;
	if (!phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

enum phase2_info1_result phase2_info1_repeat_signal_b(
	struct phase2_info1_exchange *exchange)
{
	struct phase2_info1_exchange next;
	uint8_t payload[V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];

	if (!phase2_info1_is_valid(exchange) ||
	    exchange->signal_role != PHASE2_SIGNAL_B ||
	    exchange->phase != PHASE2_INFO1_RECOVERING)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	memcpy(payload, next.transmitter.frame.payload, sizeof(payload));
	if (v34_phase2_info_audio_transmitter_init(
		    &next.transmitter, V34_CALLING_STATION,
		    next.nominal_amplitude, payload,
		    next.first_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK ||
	    v34_phase2_info_audio_receiver_init(
		    &next.receiver, V34_CALLING_STATION,
		    next.second_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next.peer_deadline_elapsed_samples = 0;
	next.carrier_tail_symbols = 0;
	next.phase = PHASE2_INFO1_TRANSMITTING;
	if (!phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

static void align_signal_a_info_with_tone(
	struct phase2_info1_exchange *exchange)
{
	struct v34_phase2_dpsk_transmitter *info =
		&exchange->transmitter.modulation;

	/* The tone oscillator uses sine and the QAM carrier uses cosine. Both
	 * phase fields identify the following sample at this boundary. */
	info->carrier.carrier_phase =
		exchange->signal_a_tone.carrier_phase - UINT32_C(0x40000000);
	info->guard_phase = exchange->signal_a_tone.guard_phase;
}

enum phase2_info1_result phase2_info1_reply_signal_a(
	struct phase2_info1_exchange *exchange,
	const uint8_t *second_payload, size_t second_payload_bit_count)
{
	struct phase2_info1_exchange next;

	if (!phase2_info1_is_valid(exchange) ||
	    exchange->signal_role != PHASE2_SIGNAL_A ||
	    exchange->phase != PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED ||
	    second_payload_bit_count != exchange->second_payload_bit_count)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	if (v34_phase2_info_audio_transmitter_init(
		    &next.transmitter, V34_ANSWERING_STATION,
		    next.nominal_amplitude, second_payload,
		    second_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	align_signal_a_info_with_tone(&next);
	next.phase = PHASE2_INFO1_TRANSMITTING;
	if (!phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO1_OK;
}

static bool receive_peer_frame(
	struct phase2_info1_exchange *exchange, int16_t sample,
	struct phase2_info1_event *event)
{
	struct v34_phase2_info_event frame;
	enum v34_phase2_info_receive_stage stage_before =
		exchange->receiver.frame.stage;

	if (v34_phase2_info_audio_receiver_sample(
		    &exchange->receiver, sample, &frame) !=
	    V34_PHASE2_INFO_AUDIO_OK)
		return false;
	event->frame_event = frame;
	if (frame.info_marks)
		event->kind = PHASE2_INFO1_EVENT_PEER_INFOMARKS;
	else if (frame.type == V34_PHASE2_INFO_FRAME_REJECTED)
		event->kind = PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
	else if (frame.type == V34_PHASE2_INFO_FRAME_RECEIVED) {
		exchange->phase = PHASE2_INFO1_PEER_FRAME_PENDING;
		event->kind = PHASE2_INFO1_EVENT_PEER_FRAME_PENDING;
	} else if (stage_before == V34_PHASE2_INFO_SEARCH_SYNC &&
		   exchange->receiver.frame.stage !=
			   V34_PHASE2_INFO_SEARCH_SYNC)
		event->kind = PHASE2_INFO1_EVENT_PEER_FRAME_STARTED;
	return true;
}

static bool generate_sequence(
	struct phase2_info1_exchange *exchange, int16_t *sample)
{
	bool frame_complete;

	if (v34_phase2_info_audio_transmitter_sample(
		    &exchange->transmitter, sample, &frame_complete) !=
	    V34_PHASE2_INFO_AUDIO_OK)
		return false;
	if (frame_complete)
		exchange->phase = PHASE2_INFO1_CARRIER_TAIL;
	return true;
}

static bool generate_carrier_tail(
	struct phase2_info1_exchange *exchange, int16_t *sample,
	struct phase2_info1_event *event)
{
	bool bit_accepted;

	if (v34_phase2_dpsk_transmitter_sample(
		    &exchange->transmitter.modulation, 0, sample,
		    &bit_accepted) != V34_PHASE2_DPSK_OK)
		return false;
	if (!bit_accepted)
		return true;
	if (++exchange->carrier_tail_symbols <
	    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS)
		return true;
	if (exchange->signal_role == PHASE2_SIGNAL_B) {
		exchange->phase = PHASE2_INFO1_WAITING_FOR_PEER;
		event->kind = PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT;
	} else {
		exchange->phase = PHASE2_INFO1_SILENCE;
		event->kind = PHASE2_INFO1_EVENT_LOCAL_SECOND_SENT;
	}
	return true;
}

static bool generate_silence(
	struct phase2_info1_exchange *exchange, int16_t *sample)
{
	*sample = 0;
	if (++exchange->silence_samples ==
	    PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES)
		exchange->phase = PHASE2_INFO1_COMPLETE;
	return true;
}

static bool generate_transmit_sample(
	struct phase2_info1_exchange *exchange, int16_t *sample,
	struct phase2_info1_event *event)
{
	switch (exchange->phase) {
	case PHASE2_INFO1_TRANSMITTING:
		return generate_sequence(exchange, sample);
	case PHASE2_INFO1_CARRIER_TAIL:
		return generate_carrier_tail(exchange, sample, event);
	case PHASE2_INFO1_WAITING_FOR_PEER:
	case PHASE2_INFO1_RECOVERING:
	case PHASE2_INFO1_PEER_FRAME_PENDING:
	case PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED:
		if (exchange->signal_role == PHASE2_SIGNAL_A)
			return v34_phase2_tone_transmitter_sample(
				       &exchange->signal_a_tone, sample) ==
			       V34_PHASE2_TONE_OK;
		*sample = 0;
		return true;
	case PHASE2_INFO1_SILENCE:
		return generate_silence(exchange, sample);
	case PHASE2_INFO1_COMPLETE:
	case PHASE2_INFO1_FAILED:
		*sample = 0;
		return true;
	}
	return false;
}

enum phase2_info1_result phase2_info1_sample(
	struct phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_info1_event *event)
{
	struct phase2_info1_exchange next;
	struct phase2_info1_event emitted = { 0 };
	int16_t generated = 0;
	bool waited_for_peer;

	if (!phase2_info1_is_valid(exchange) || transmit_sample == NULL ||
	    event == NULL || exchange->phase == PHASE2_INFO1_PEER_FRAME_PENDING)
		return PHASE2_INFO1_INVALID_ARGUMENT;
	next = *exchange;
	waited_for_peer = phase2_info1_is_waiting_for_peer(&next);
	if ((waited_for_peer &&
	     !receive_peer_frame(&next, received_sample, &emitted)) ||
	    !generate_transmit_sample(&next, &generated, &emitted))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	if (waited_for_peer && exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER &&
	    next.phase == PHASE2_INFO1_WAITING_FOR_PEER) {
		advance_peer_deadline(&next);
		if (next.phase == PHASE2_INFO1_FAILED) {
			emitted.kind = PHASE2_INFO1_EVENT_FAILED;
			emitted.failure = next.failure;
		}
	}
	if (!phase2_info1_is_valid(&next))
		return PHASE2_INFO1_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INFO1_OK;
}

const char *phase2_info1_result_name(enum phase2_info1_result result)
{
	switch (result) {
	case PHASE2_INFO1_OK:
		return "ok";
	case PHASE2_INFO1_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
