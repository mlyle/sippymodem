// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_info0_exchange.h"

static bool transmit_phase_is_valid(
	enum phase2_info0_transmit_phase phase)
{
	return phase == PHASE2_INFO0_POST_V8_SILENCE ||
	       phase == PHASE2_INFO0_SEQUENCE ||
	       phase == PHASE2_INFO0_CARRIER_TAIL ||
	       phase == PHASE2_INFO0_TONE;
}

bool phase2_info0_exchange_is_valid(
	const struct phase2_info0_exchange *exchange)
{
	enum v34_station_role station_role;

	if (exchange == NULL ||
	    !phase2_signal_role_is_valid(exchange->signal_role) ||
	    !transmit_phase_is_valid(exchange->transmit_phase) ||
	    exchange->post_v8_silence_samples >
		    PHASE2_INFO0_POST_V8_SILENCE_SAMPLES ||
	    exchange->filter_tail_symbols >
		    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS ||
	    !v34_phase2_info_audio_transmitter_is_valid(
		    &exchange->info_transmitter) ||
	    !v34_phase2_info_audio_receiver_is_valid(
		    &exchange->info_receiver) ||
	    !v34_phase2_tone_transmitter_is_valid(
		    &exchange->tone_transmitter) ||
	    !v34_phase2_tone_detector_is_valid(
		    &exchange->tone_detector))
		return false;
	station_role = v34_phase2_station_for_signal(exchange->signal_role);
	if (exchange->info_transmitter.frame.payload_bit_count == 0 ||
	    exchange->info_receiver.frame.expected_payload_bit_count == 0 ||
	    exchange->info_transmitter.modulation.local_role != station_role ||
	    exchange->info_receiver.modulation.local_role != station_role ||
	    exchange->tone_transmitter.local_role != station_role ||
	    exchange->tone_detector.local_role != station_role)
		return false;

	switch (exchange->transmit_phase) {
	case PHASE2_INFO0_POST_V8_SILENCE:
		return exchange->post_v8_silence_samples <
			       PHASE2_INFO0_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols == 0 &&
		       !exchange->info_transmitter.frame_complete;
	case PHASE2_INFO0_SEQUENCE:
		return exchange->post_v8_silence_samples ==
			       PHASE2_INFO0_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols == 0 &&
		       !exchange->info_transmitter.frame_complete;
	case PHASE2_INFO0_CARRIER_TAIL:
		return exchange->post_v8_silence_samples ==
			       PHASE2_INFO0_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols <
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       exchange->info_transmitter.frame_complete;
	case PHASE2_INFO0_TONE:
		return exchange->post_v8_silence_samples ==
			       PHASE2_INFO0_POST_V8_SILENCE_SAMPLES &&
		       exchange->filter_tail_symbols ==
			       V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
		       exchange->info_transmitter.frame_complete;
	}
	return false;
}

enum phase2_info0_exchange_result phase2_info0_exchange_init(
	struct phase2_info0_exchange *exchange,
	enum phase2_signal_role signal_role,
	int16_t nominal_amplitude,
	const uint8_t *local_payload, size_t local_payload_bit_count,
	size_t peer_payload_bit_count)
{
	struct phase2_info0_exchange initialized = {
		.transmit_phase = PHASE2_INFO0_POST_V8_SILENCE,
		.signal_role = signal_role,
	};
	enum v34_station_role station_role;

	if (exchange == NULL || !phase2_signal_role_is_valid(signal_role) ||
	    local_payload == NULL || local_payload_bit_count == 0 ||
	    peer_payload_bit_count == 0)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	station_role = v34_phase2_station_for_signal(signal_role);
	if (v34_phase2_info_audio_transmitter_init(
		    &initialized.info_transmitter, station_role,
		    nominal_amplitude, local_payload,
		    local_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK ||
	    v34_phase2_info_audio_receiver_init(
		    &initialized.info_receiver, station_role,
		    peer_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK ||
	    v34_phase2_tone_transmitter_init(
		    &initialized.tone_transmitter, station_role,
		    nominal_amplitude) != V34_PHASE2_TONE_OK ||
	    v34_phase2_tone_detector_init(
		    &initialized.tone_detector, station_role) !=
		    V34_PHASE2_TONE_OK)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	/* The INFO carrier uses cosine while the following tone uses sine.
	 * Clock both oscillators from the same next-sample phase so a reversal
	 * requested during a recovery record survives its eventual handoff. */
	initialized.tone_transmitter.carrier_phase =
		initialized.info_transmitter.modulation.carrier.carrier_phase +
		UINT32_C(0x40000000);
	initialized.tone_transmitter.guard_phase =
		initialized.info_transmitter.modulation.guard_phase;
	if (!phase2_info0_exchange_is_valid(&initialized))
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return PHASE2_INFO0_EXCHANGE_OK;
}

enum phase2_info0_exchange_result phase2_info0_exchange_restart_info(
	struct phase2_info0_exchange *exchange,
	const uint8_t *local_payload, size_t local_payload_bit_count)
{
	struct phase2_info0_exchange restarted;
	struct v34_phase2_info_audio_transmitter transmitter;

	if (!phase2_info0_exchange_is_valid(exchange) ||
	    exchange->transmit_phase != PHASE2_INFO0_TONE ||
	    local_payload == NULL || local_payload_bit_count == 0)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	if (local_payload_bit_count !=
	    exchange->info_transmitter.frame.payload_bit_count)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	if (v34_phase2_info_audio_transmitter_init(
		    &transmitter,
		    v34_phase2_station_for_signal(exchange->signal_role),
		    v34_phase2_tone_nominal_amplitude(&exchange->tone_transmitter),
		    local_payload,
		    local_payload_bit_count) != V34_PHASE2_INFO_AUDIO_OK)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;

	/* Tone uses sine and QAM uses cosine.  Both stored phases already point
	 * at the next line sample, so removing one quarter turn preserves the
	 * carrier reference while the new pulse shaper starts its fill. */
	transmitter.modulation.carrier.carrier_phase =
		exchange->tone_transmitter.carrier_phase -
		UINT32_C(0x40000000);
	transmitter.modulation.guard_phase =
		exchange->tone_transmitter.guard_phase;
	restarted = *exchange;
	restarted.info_transmitter = transmitter;
	restarted.filter_tail_symbols = 0;
	restarted.transmit_phase = PHASE2_INFO0_SEQUENCE;
	if (!phase2_info0_exchange_is_valid(&restarted))
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = restarted;
	return PHASE2_INFO0_EXCHANGE_OK;
}

enum phase2_info0_exchange_result phase2_info0_exchange_start_tone(
	struct phase2_info0_exchange *exchange)
{
	struct phase2_info0_exchange next;
	size_t frame_bits;

	if (!phase2_info0_exchange_is_valid(exchange) ||
	    exchange->transmit_phase != PHASE2_INFO0_POST_V8_SILENCE ||
	    exchange->post_v8_silence_samples != 0)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	frame_bits = v34_phase2_info_frame_bit_count(
		next.info_transmitter.frame.payload_bit_count);
	while (next.info_transmitter.frame.bit_index < frame_bits) {
		uint8_t bit;
		bool complete;

		if (v34_phase2_info_next_bit(
			    &next.info_transmitter.frame, &bit, &complete) !=
			    V34_PHASE2_INFO_OK)
			return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
		(void)bit;
		if (complete)
			next.info_transmitter.frame_complete = true;
	}
	next.post_v8_silence_samples = PHASE2_INFO0_POST_V8_SILENCE_SAMPLES;
	next.filter_tail_symbols = V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS;
	next.transmit_phase = PHASE2_INFO0_TONE;
	if (!phase2_info0_exchange_is_valid(&next))
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	return PHASE2_INFO0_EXCHANGE_OK;
}

static bool receive_sample(
	struct phase2_info0_exchange *exchange, int16_t sample,
	struct phase2_info0_exchange_event *event)
{
	enum v34_phase2_info_receive_stage stage_before =
		exchange->info_receiver.frame.stage;
	bool info_carrier_active;

	if (v34_phase2_info_audio_receiver_sample(
		    &exchange->info_receiver, sample,
		    &event->info_event) != V34_PHASE2_INFO_AUDIO_OK)
		return false;
	info_carrier_active =
		stage_before != V34_PHASE2_INFO_SEARCH_SYNC ||
		exchange->info_receiver.frame.stage !=
			V34_PHASE2_INFO_SEARCH_SYNC;
	if (!info_carrier_active)
		return v34_phase2_tone_detector_sample(
			       &exchange->tone_detector, sample,
			       &event->tone_event) == V34_PHASE2_TONE_OK;
	/* A synchronized INFO frame temporarily owns the shared carrier. Keep
	 * its samples out of the tone detector while advancing the oscillator,
	 * so the following tone is compared with the qualified pre-frame phase. */
	if (v34_phase2_tone_detector_suppress_sample(
		    &exchange->tone_detector) != V34_PHASE2_TONE_OK)
		return false;
	return exchange->info_receiver.frame.stage !=
		       V34_PHASE2_INFO_SEARCH_SYNC ||
	       v34_phase2_tone_detector_resume(
		       &exchange->tone_detector,
		       v34_phase2_info_audio_receiver_handoff_delay_samples(
			       &exchange->info_receiver)) ==
		       V34_PHASE2_TONE_OK;
}

static void align_tone_with_info_carrier(
	struct phase2_info0_exchange *exchange)
{
	const struct v34_phase2_dpsk_transmitter *info =
		&exchange->info_transmitter.modulation;
	struct v34_phase2_tone_transmitter *tone =
		&exchange->tone_transmitter;

	/* Both oscillators were advanced together. Apply only the final DPSK
	 * point inversion; any ranging reversal made while INFO was on the wire
	 * is already retained by the tone oscillator. */
	if (info->point_inverted)
		tone->carrier_phase += UINT32_C(0x80000000);
}

static void advance_tone_oscillator(
	struct phase2_info0_exchange *exchange)
{
	exchange->tone_transmitter.carrier_phase +=
		exchange->tone_transmitter.carrier_phase_step;
	exchange->tone_transmitter.guard_phase +=
		exchange->tone_transmitter.guard_phase_step;
}

static bool generate_transmit_sample(
	struct phase2_info0_exchange *exchange, int16_t *sample)
{
	switch (exchange->transmit_phase) {
	case PHASE2_INFO0_POST_V8_SILENCE:
		*sample = 0;
		if (++exchange->post_v8_silence_samples ==
		    PHASE2_INFO0_POST_V8_SILENCE_SAMPLES)
			exchange->transmit_phase = PHASE2_INFO0_SEQUENCE;
		return true;
	case PHASE2_INFO0_SEQUENCE: {
		bool complete;

		if (v34_phase2_info_audio_transmitter_sample(
			    &exchange->info_transmitter, sample, &complete) !=
		    V34_PHASE2_INFO_AUDIO_OK)
			return false;
		advance_tone_oscillator(exchange);
		if (complete)
			exchange->transmit_phase = PHASE2_INFO0_CARRIER_TAIL;
		return true;
	}
	case PHASE2_INFO0_CARRIER_TAIL: {
		bool bit_accepted;

		if (v34_phase2_dpsk_transmitter_sample(
			    &exchange->info_transmitter.modulation, 0, sample,
			    &bit_accepted) != V34_PHASE2_DPSK_OK)
			return false;
		advance_tone_oscillator(exchange);
		if (bit_accepted &&
		    ++exchange->filter_tail_symbols ==
			    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS) {
			align_tone_with_info_carrier(exchange);
			exchange->transmit_phase = PHASE2_INFO0_TONE;
		}
		return true;
	}
	case PHASE2_INFO0_TONE:
		return v34_phase2_tone_transmitter_sample(
			       &exchange->tone_transmitter, sample) ==
		       V34_PHASE2_TONE_OK;
	}
	return false;
}

enum phase2_info0_exchange_result phase2_info0_exchange_sample(
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_info0_exchange_event *event)
{
	struct phase2_info0_exchange next;
	struct phase2_info0_exchange_event emitted = { 0 };
	int16_t generated;

	if (!phase2_info0_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	next = *exchange;
	if (!receive_sample(&next, received_sample, &emitted) ||
	    !generate_transmit_sample(&next, &generated) ||
	    !phase2_info0_exchange_is_valid(&next))
		return PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT;
	*exchange = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INFO0_EXCHANGE_OK;
}
