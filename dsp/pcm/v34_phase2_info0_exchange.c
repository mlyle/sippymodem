// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info0_exchange.h"

#include <string.h>

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static bool transmit_phase_is_valid(
	enum v34_phase2_info0_transmit_phase phase)
{
	return phase == V34_PHASE2_INFO0_POST_V8_SILENCE ||
	       phase == V34_PHASE2_INFO0_SEQUENCE ||
	       phase == V34_PHASE2_INFO0_CARRIER_TAIL ||
	       phase == V34_PHASE2_INFO0_TONE;
}

bool v34_phase2_info0_exchange_is_valid(
	const struct v34_phase2_info0_exchange *exchange)
{
	if (exchange == NULL || !role_is_valid(exchange->local_role) ||
	    !transmit_phase_is_valid(exchange->transmit_phase) ||
	    exchange->post_v8_silence_samples >
		    V34_PHASE2_POST_V8_SILENCE_SAMPLES ||
	    exchange->filter_tail_symbols >
		    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS ||
	    !v34_phase2_info_audio_transmitter_is_valid(
		    &exchange->info_transmitter) ||
	    !v34_phase2_info_audio_receiver_is_valid(
		    &exchange->info_receiver) ||
	    !v34_phase2_tone_transmitter_is_valid(
		    &exchange->tone_transmitter) ||
	    !v34_phase2_tone_detector_is_valid(
		    &exchange->tone_detector) ||
	    (exchange->peer_info0_received &&
	     !v34_info0_is_valid(&exchange->peer_info0)))
		return false;

	switch (exchange->transmit_phase) {
	case V34_PHASE2_INFO0_POST_V8_SILENCE:
		return exchange->post_v8_silence_samples <
			       V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols == 0 &&
		       !exchange->info_transmitter.frame_complete;
	case V34_PHASE2_INFO0_SEQUENCE:
		return exchange->post_v8_silence_samples ==
			       V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols == 0 &&
		       !exchange->info_transmitter.frame_complete;
	case V34_PHASE2_INFO0_CARRIER_TAIL:
		return exchange->post_v8_silence_samples ==
			       V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols <
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       exchange->info_transmitter.frame_complete;
	case V34_PHASE2_INFO0_TONE:
		return exchange->post_v8_silence_samples ==
			       V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       exchange->info_transmitter.frame_complete;
	}
	return false;
}

enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_init(
	struct v34_phase2_info0_exchange *exchange,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0)
{
	struct v34_phase2_info0_exchange initialized = {
		.transmit_phase =
			V34_PHASE2_INFO0_POST_V8_SILENCE,
		.local_role = local_role,
	};
	struct v34_info0 initial_info0;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];

	if (exchange == NULL || !role_is_valid(local_role) ||
	    !v34_info0_is_valid(local_info0))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial_info0 = *local_info0;
	initial_info0.acknowledges_peer_info0 = false;
	if (v34_info0_encode(
		    &initial_info0, payload, sizeof(payload)) !=
		    V34_INFO0_OK ||
	    v34_phase2_info_audio_transmitter_init(
		    &initialized.info_transmitter, local_role,
		    nominal_amplitude, payload,
		    V34_INFO0_PAYLOAD_BITS) !=
		    V34_PHASE2_INFO_AUDIO_OK ||
	    v34_phase2_info_audio_receiver_init(
		    &initialized.info_receiver, local_role,
		    V34_INFO0_PAYLOAD_BITS) !=
		    V34_PHASE2_INFO_AUDIO_OK ||
	    v34_phase2_tone_transmitter_init(
		    &initialized.tone_transmitter, local_role,
		    nominal_amplitude) != V34_PHASE2_TONE_OK ||
	    v34_phase2_tone_detector_init(
		    &initialized.tone_detector, local_role) !=
		    V34_PHASE2_TONE_OK ||
	    !v34_phase2_info0_exchange_is_valid(&initialized))
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return V34_PHASE2_INFO0_EXCHANGE_OK;
}

static bool receive_info0(
	struct v34_phase2_info0_exchange *exchange,
	int16_t sample,
	struct v34_phase2_info0_exchange_event *event)
{
	struct v34_phase2_info_event frame_event;
	struct v34_info0 decoded;

	if (v34_phase2_info_audio_receiver_sample(
		    &exchange->info_receiver, sample, &frame_event) !=
	    V34_PHASE2_INFO_AUDIO_OK)
		return false;
	if (frame_event.type == V34_PHASE2_INFO_FRAME_REJECTED) {
		event->peer_info0_event =
			V34_PHASE2_PEER_INFO0_REJECTED;
		return true;
	}
	if (frame_event.type != V34_PHASE2_INFO_FRAME_RECEIVED)
		return true;
	if (v34_info0_decode(
		    frame_event.payload, sizeof(frame_event.payload),
		    frame_event.payload_bit_count, &decoded) !=
	    V34_INFO0_OK) {
		event->peer_info0_event =
			V34_PHASE2_PEER_INFO0_REJECTED;
		return true;
	}
	exchange->peer_info0 = decoded;
	exchange->peer_info0_received = true;
	event->peer_info0_event =
		V34_PHASE2_PEER_INFO0_RECEIVED;
	event->peer_info0 = decoded;
	return true;
}

static bool receive_sample(
	struct v34_phase2_info0_exchange *exchange,
	int16_t sample,
	struct v34_phase2_info0_exchange_event *event)
{
	return receive_info0(exchange, sample, event) &&
	       v34_phase2_tone_detector_sample(
		       &exchange->tone_detector, sample,
		       &event->tone_event) == V34_PHASE2_TONE_OK;
}

static void align_tone_with_info_carrier(
	struct v34_phase2_info0_exchange *exchange)
{
	const struct v34_phase2_dpsk_transmitter *info =
		&exchange->info_transmitter.modulation;
	struct v34_phase2_tone_transmitter *tone =
		&exchange->tone_transmitter;

	/* The QAM carrier represents its in-phase point with cosine, while the
	 * tone oscillator represents it with sine. Add one quarter turn and the
	 * final DPSK point inversion so the handoff does not create a false edge.
	 * The answering guard uses sine in both implementations. */
	tone->carrier_phase = info->carrier.carrier_phase +
		UINT32_C(0x40000000) +
		(info->point_inverted ? UINT32_C(0x80000000) : 0);
	tone->guard_phase = info->guard_phase;
}

static bool generate_transmit_sample(
	struct v34_phase2_info0_exchange *exchange,
	int16_t *sample)
{
	switch (exchange->transmit_phase) {
	case V34_PHASE2_INFO0_POST_V8_SILENCE:
		*sample = 0;
		if (++exchange->post_v8_silence_samples ==
		    V34_PHASE2_POST_V8_SILENCE_SAMPLES)
			exchange->transmit_phase =
				V34_PHASE2_INFO0_SEQUENCE;
		return true;
	case V34_PHASE2_INFO0_SEQUENCE: {
		bool complete;

		if (v34_phase2_info_audio_transmitter_sample(
			    &exchange->info_transmitter, sample, &complete) !=
		    V34_PHASE2_INFO_AUDIO_OK)
			return false;
		if (complete)
			exchange->transmit_phase =
				V34_PHASE2_INFO0_CARRIER_TAIL;
		return true;
	}
	case V34_PHASE2_INFO0_CARRIER_TAIL: {
		bool bit_accepted;

		if (v34_phase2_dpsk_transmitter_sample(
			    &exchange->info_transmitter.modulation, 0, sample,
			    &bit_accepted) != V34_PHASE2_DPSK_OK)
			return false;
		if (bit_accepted) {
			exchange->filter_tail_symbols++;
			if (exchange->filter_tail_symbols ==
			    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS) {
				align_tone_with_info_carrier(exchange);
				exchange->transmit_phase =
					V34_PHASE2_INFO0_TONE;
			}
		}
		return true;
	}
	case V34_PHASE2_INFO0_TONE:
		return v34_phase2_tone_transmitter_sample(
			       &exchange->tone_transmitter, sample) ==
		       V34_PHASE2_TONE_OK;
	}
	return false;
}

enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_sample(
	struct v34_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info0_exchange_event *event)
{
	struct v34_phase2_info0_exchange next;
	struct v34_phase2_info0_exchange_event emitted = { 0 };
	int16_t generated;

	if (!v34_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (!receive_sample(&next, received_sample, &emitted) ||
	    !generate_transmit_sample(&next, &generated) ||
	    !v34_phase2_info0_exchange_is_valid(&next))
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
