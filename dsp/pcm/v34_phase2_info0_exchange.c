// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_info0_exchange.h"

#include <string.h>

static bool role_is_valid(enum sippymodem_v34_station_role role)
{
	return role == SIPPYMODEM_V34_CALLING_STATION ||
	       role == SIPPYMODEM_V34_ANSWERING_STATION;
}

static bool transmit_phase_is_valid(
	enum sippymodem_v34_phase2_info0_transmit_phase phase)
{
	return phase == SIPPYMODEM_V34_PHASE2_INFO0_POST_V8_SILENCE ||
	       phase == SIPPYMODEM_V34_PHASE2_INFO0_SEQUENCE ||
	       phase == SIPPYMODEM_V34_PHASE2_INFO0_CARRIER_TAIL ||
	       phase == SIPPYMODEM_V34_PHASE2_INFO0_TONE;
}

bool sippymodem_v34_phase2_info0_exchange_is_valid(
	const struct sippymodem_v34_phase2_info0_exchange *exchange)
{
	if (exchange == NULL || !role_is_valid(exchange->local_role) ||
	    !transmit_phase_is_valid(exchange->transmit_phase) ||
	    exchange->post_v8_silence_samples >
		    SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES ||
	    exchange->filter_tail_symbols >
		    SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS ||
	    !sippymodem_v34_phase2_info_audio_transmitter_is_valid(
		    &exchange->info_transmitter) ||
	    !sippymodem_v34_phase2_info_audio_receiver_is_valid(
		    &exchange->info_receiver) ||
	    !sippymodem_v34_phase2_tone_transmitter_is_valid(
		    &exchange->tone_transmitter) ||
	    !sippymodem_v34_phase2_tone_detector_is_valid(
		    &exchange->tone_detector) ||
	    (exchange->peer_info0_received &&
	     !sippymodem_v34_info0_is_valid(&exchange->peer_info0)))
		return false;

	switch (exchange->transmit_phase) {
	case SIPPYMODEM_V34_PHASE2_INFO0_POST_V8_SILENCE:
		return exchange->post_v8_silence_samples <
			       SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols == 0 &&
		       !exchange->info_transmitter.frame_complete;
	case SIPPYMODEM_V34_PHASE2_INFO0_SEQUENCE:
		return exchange->post_v8_silence_samples ==
			       SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols == 0 &&
		       !exchange->info_transmitter.frame_complete;
	case SIPPYMODEM_V34_PHASE2_INFO0_CARRIER_TAIL:
		return exchange->post_v8_silence_samples ==
			       SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols <
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       exchange->info_transmitter.frame_complete;
	case SIPPYMODEM_V34_PHASE2_INFO0_TONE:
		return exchange->post_v8_silence_samples ==
			       SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols ==
			       SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       exchange->info_transmitter.frame_complete;
	}
	return false;
}

enum sippymodem_v34_phase2_info0_exchange_result
sippymodem_v34_phase2_info0_exchange_init(
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	enum sippymodem_v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct sippymodem_v34_info0 *local_info0)
{
	struct sippymodem_v34_phase2_info0_exchange initialized = {
		.transmit_phase =
			SIPPYMODEM_V34_PHASE2_INFO0_POST_V8_SILENCE,
		.local_role = local_role,
	};
	struct sippymodem_v34_info0 initial_info0;
	uint8_t payload[SIPPYMODEM_V34_INFO0_PAYLOAD_BYTES];

	if (exchange == NULL || !role_is_valid(local_role) ||
	    !sippymodem_v34_info0_is_valid(local_info0))
		return SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	initial_info0 = *local_info0;
	initial_info0.acknowledges_peer_info0 = false;
	if (sippymodem_v34_info0_encode(
		    &initial_info0, payload, sizeof(payload)) !=
		    SIPPYMODEM_V34_INFO0_OK ||
	    sippymodem_v34_phase2_info_audio_transmitter_init(
		    &initialized.info_transmitter, local_role,
		    nominal_amplitude, payload,
		    SIPPYMODEM_V34_INFO0_PAYLOAD_BITS) !=
		    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK ||
	    sippymodem_v34_phase2_info_audio_receiver_init(
		    &initialized.info_receiver, local_role,
		    SIPPYMODEM_V34_INFO0_PAYLOAD_BITS) !=
		    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK ||
	    sippymodem_v34_phase2_tone_transmitter_init(
		    &initialized.tone_transmitter, local_role,
		    nominal_amplitude) != SIPPYMODEM_V34_PHASE2_TONE_OK ||
	    sippymodem_v34_phase2_tone_detector_init(
		    &initialized.tone_detector, local_role) !=
		    SIPPYMODEM_V34_PHASE2_TONE_OK ||
	    !sippymodem_v34_phase2_info0_exchange_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_OK;
}

static bool receive_info0(
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	int16_t sample,
	struct sippymodem_v34_phase2_info0_exchange_event *event)
{
	struct sippymodem_v34_phase2_info_event frame_event;
	struct sippymodem_v34_info0 decoded;

	if (sippymodem_v34_phase2_info_audio_receiver_sample(
		    &exchange->info_receiver, sample, &frame_event) !=
	    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK)
		return false;
	if (frame_event.type == SIPPYMODEM_V34_PHASE2_INFO_FRAME_REJECTED) {
		event->peer_info0_event =
			SIPPYMODEM_V34_PHASE2_PEER_INFO0_REJECTED;
		return true;
	}
	if (frame_event.type != SIPPYMODEM_V34_PHASE2_INFO_FRAME_RECEIVED)
		return true;
	if (sippymodem_v34_info0_decode(
		    frame_event.payload, sizeof(frame_event.payload),
		    frame_event.payload_bit_count, &decoded) !=
	    SIPPYMODEM_V34_INFO0_OK) {
		event->peer_info0_event =
			SIPPYMODEM_V34_PHASE2_PEER_INFO0_REJECTED;
		return true;
	}
	exchange->peer_info0 = decoded;
	exchange->peer_info0_received = true;
	event->peer_info0_event =
		SIPPYMODEM_V34_PHASE2_PEER_INFO0_RECEIVED;
	event->peer_info0 = decoded;
	return true;
}

static bool receive_sample(
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	int16_t sample,
	struct sippymodem_v34_phase2_info0_exchange_event *event)
{
	return receive_info0(exchange, sample, event) &&
	       sippymodem_v34_phase2_tone_detector_sample(
		       &exchange->tone_detector, sample,
		       &event->tone_event) == SIPPYMODEM_V34_PHASE2_TONE_OK;
}

static void align_tone_with_info_carrier(
	struct sippymodem_v34_phase2_info0_exchange *exchange)
{
	const struct sippymodem_v34_phase2_dpsk_transmitter *info =
		&exchange->info_transmitter.modulation;
	struct sippymodem_v34_phase2_tone_transmitter *tone =
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
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	int16_t *sample)
{
	switch (exchange->transmit_phase) {
	case SIPPYMODEM_V34_PHASE2_INFO0_POST_V8_SILENCE:
		*sample = 0;
		if (++exchange->post_v8_silence_samples ==
		    SIPPYMODEM_V34_PHASE2_POST_V8_SILENCE_SAMPLES)
			exchange->transmit_phase =
				SIPPYMODEM_V34_PHASE2_INFO0_SEQUENCE;
		return true;
	case SIPPYMODEM_V34_PHASE2_INFO0_SEQUENCE: {
		bool complete;

		if (sippymodem_v34_phase2_info_audio_transmitter_sample(
			    &exchange->info_transmitter, sample, &complete) !=
		    SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK)
			return false;
		if (complete)
			exchange->transmit_phase =
				SIPPYMODEM_V34_PHASE2_INFO0_CARRIER_TAIL;
		return true;
	}
	case SIPPYMODEM_V34_PHASE2_INFO0_CARRIER_TAIL: {
		bool bit_accepted;

		if (sippymodem_v34_phase2_dpsk_transmitter_sample(
			    &exchange->info_transmitter.modulation, 0, sample,
			    &bit_accepted) != SIPPYMODEM_V34_PHASE2_DPSK_OK)
			return false;
		if (bit_accepted) {
			exchange->filter_tail_symbols++;
			if (exchange->filter_tail_symbols ==
			    SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS) {
				align_tone_with_info_carrier(exchange);
				exchange->transmit_phase =
					SIPPYMODEM_V34_PHASE2_INFO0_TONE;
			}
		}
		return true;
	}
	case SIPPYMODEM_V34_PHASE2_INFO0_TONE:
		return sippymodem_v34_phase2_tone_transmitter_sample(
			       &exchange->tone_transmitter, sample) ==
		       SIPPYMODEM_V34_PHASE2_TONE_OK;
	}
	return false;
}

enum sippymodem_v34_phase2_info0_exchange_result
sippymodem_v34_phase2_info0_exchange_sample(
	struct sippymodem_v34_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_info0_exchange_event *event)
{
	struct sippymodem_v34_phase2_info0_exchange next;
	struct sippymodem_v34_phase2_info0_exchange_event emitted = { 0 };
	int16_t generated;

	if (!sippymodem_v34_phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (!receive_sample(&next, received_sample, &emitted) ||
	    !generate_transmit_sample(&next, &generated) ||
	    !sippymodem_v34_phase2_info0_exchange_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_OK;
}

const char *sippymodem_v34_phase2_info0_exchange_result_name(
	enum sippymodem_v34_phase2_info0_exchange_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
