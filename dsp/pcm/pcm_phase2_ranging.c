// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_ranging.h"

#include "pcm_startup.h"

#include <stddef.h>

static bool short_agreed(const struct pcm_phase2_info0_exchange *exchange)
{
	const struct v34_phase2_info_generator *frame =
		&exchange->transport.info_transmitter.frame;
	struct pcm_startup_selection selected;
	struct v92_info0a a;
	struct v92_info0d d;

	if (exchange->protocol != PCM_PROTOCOL_V92 ||
	    exchange->state == PCM_PHASE2_INFO0_INITIAL ||
	    exchange->state == PCM_PHASE2_INFO0_REPEAT)
		return false;
	if (exchange->local_role == V90_ANALOG_MODEM) {
		if (v92_info0a_decode(frame->payload, sizeof(frame->payload),
			    frame->payload_bit_count, &a) != V92_INFO0_OK)
			return false;
		d = exchange->peer_info0.v92_digital;
	} else {
		if (v92_info0d_decode(frame->payload, sizeof(frame->payload),
			    frame->payload_bit_count, &d) != V92_INFO0_OK)
			return false;
		a = exchange->peer_info0.v92_analog;
	}
	return pcm_startup_select(&a, &d, &selected) == PCM_STARTUP_OK &&
	       selected.phase2 == PCM_PHASE2_SHORT;
}

static bool uses_short_control(const struct pcm_phase2_ranging *ranging)
{
	return ranging->path == PCM_RANGING_SHORT_PENDING ||
	       ranging->path == PCM_RANGING_SHORT_ACTIVE ||
	       ranging->path == PCM_RANGING_SHORT_FINISHED;
}

static bool ranging_context_is_retained(
	const struct pcm_phase2_ranging *ranging)
{
	switch (ranging->info0_exchange.state) {
	case PCM_PHASE2_INFO0_INITIAL:
	case PCM_PHASE2_INFO0_REPEAT:
	case PCM_PHASE2_INFO0_VERIFY_REPEAT:
		return ranging->control.phase != PHASE2_RANGING_INFO0_EXCHANGE;
	case PCM_PHASE2_INFO0_ACCEPTED:
	case PCM_PHASE2_INFO0_ACKNOWLEDGING:
	case PCM_PHASE2_INFO0_ACK_REPEAT:
	case PCM_PHASE2_INFO0_WAIT_COMPLETION:
	case PCM_PHASE2_INFO0_COMPLETE:
	case PCM_PHASE2_INFO0_RETRAIN:
		return true;
	}
	return false;
}

bool pcm_phase2_ranging_is_valid(
	const struct pcm_phase2_ranging *ranging)
{
	if (ranging == NULL || (unsigned)ranging->path > PCM_RANGING_SHORT_FULL_RECOVERY ||
	    ranging->actual_tone_samples > PHASE2_RANGING_INITIAL_TONE_SAMPLES ||
	    ranging->since_info0_samples > V92_SHORT_RANGING_TIMEOUT_SAMPLES ||
	    !pcm_phase2_info0_exchange_is_valid(&ranging->info0_exchange))
		return false;
	if (uses_short_control(ranging)) {
		if (!short_agreed(&ranging->info0_exchange))
			return false;
		if (ranging->path == PCM_RANGING_SHORT_PENDING)
			return true;
		return (ranging->info0_exchange.state == PCM_PHASE2_INFO0_COMPLETE ||
			    ranging->info0_exchange.state == PCM_PHASE2_INFO0_VERIFY_REPEAT) &&
		       v92_short_ranging_is_valid(&ranging->short_control) &&
		       ranging->short_control.signal_role == ranging->info0_exchange.transport.signal_role &&
		       ((ranging->short_control.phase == V92_SHORT_RANGING_COMPLETE) ==
			(ranging->path == PCM_RANGING_SHORT_FINISHED));
	}
	if (ranging->path == PCM_RANGING_SHORT_FULL_RECOVERY &&
	    !short_agreed(&ranging->info0_exchange))
		return false;
	return phase2_ranging_control_is_valid(
		&ranging->control, &ranging->info0_exchange.transport,
		ranging_context_is_retained(ranging));
}

static bool enter_full_recovery(struct pcm_phase2_ranging *ranging, bool info1_timeout)
{
	struct phase2_info0_exchange *transport = &ranging->info0_exchange.transport;
	bool have_tone = transport->tone_detector.carrier.tone_present;
	struct phase2_ranging_control full = {
		.phase = info1_timeout || have_tone ? PHASE2_WAIT_FIRST_REMOTE_REVERSAL :
			PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE,
	};

	if (info1_timeout || !have_tone) {
		if (v34_phase2_tone_detector_reset(&transport->tone_detector) != V34_PHASE2_TONE_OK ||
		    (ranging->info0_exchange.state != PCM_PHASE2_INFO0_VERIFY_REPEAT &&
		     v34_phase2_info_audio_receiver_init(&transport->info_receiver,
			    v34_phase2_station_for_signal(transport->signal_role),
			    transport->info_receiver.frame.expected_payload_bit_count) !=
			    V34_PHASE2_INFO_AUDIO_OK))
			return false;
	}
	ranging->control = full;
	ranging->path = PCM_RANGING_SHORT_FULL_RECOVERY;
	return pcm_phase2_ranging_is_valid(ranging);
}

bool pcm_phase2_ranging_recover_short_info1(struct pcm_phase2_ranging *ranging)
{
	struct pcm_phase2_ranging next;

	if (!pcm_phase2_ranging_is_valid(ranging) ||
	    ranging->path != PCM_RANGING_SHORT_FINISHED ||
	    ranging->info0_exchange.local_role != V90_DIGITAL_MODEM ||
	    ranging->short_control.elapsed_samples >= V92_SHORT_RANGING_TIMEOUT_SAMPLES)
		return false;
	next = *ranging;
	if (!enter_full_recovery(&next, true))
		return false;
	*ranging = next;
	return true;
}

static void count_short_timing(struct pcm_phase2_ranging *ranging,
	enum phase2_info0_transmit_phase before)
{
	enum phase2_info0_transmit_phase after = ranging->info0_exchange.transport.transmit_phase;

	if (before == PHASE2_INFO0_TONE && after == PHASE2_INFO0_TONE) {
		if (ranging->actual_tone_samples < PHASE2_RANGING_INITIAL_TONE_SAMPLES)
			++ranging->actual_tone_samples;
	} else if (after != PHASE2_INFO0_TONE) {
		ranging->actual_tone_samples = 0;
	}
	/* INFO0 is still on the line during the pulse-shaper tail. Date the
	 * analogue timeout from its waveform handoff, not the last queued bit. */
	if (before != PHASE2_INFO0_TONE || after != PHASE2_INFO0_TONE)
		ranging->since_info0_samples = 0;
	else if (ranging->since_info0_samples < V92_SHORT_RANGING_TIMEOUT_SAMPLES)
		++ranging->since_info0_samples;
}

static bool finish_short_sample(struct pcm_phase2_ranging *ranging,
	struct v34_phase2_tone_transmitter *tone_before,
	const struct pcm_phase2_info0_exchange_event *info_event, int16_t *generated,
	struct v92_short_ranging_step *step)
{
	struct pcm_phase2_info0_exchange *exchange = &ranging->info0_exchange;

	/* A repeated INFO0 can overlap the 40 ms response or its 10 ms tail.
	 * Stop that turn as soon as framing owns the peer carrier: waiting for
	 * its CRC would let us advance to INFO1a before recovery is decided.
	 * This cancels a pending edge, not the original 2500 ms deadline. */
	if (info_event->recovery_event == PCM_INFO0_RECOVERY_STARTED &&
	    ranging->path == PCM_RANGING_SHORT_ACTIVE &&
	    ranging->short_control.signal_role == PHASE2_SIGNAL_A &&
	    (ranging->short_control.phase == V92_SHORT_RANGING_ANALOG_DELAY ||
	     ranging->short_control.phase == V92_SHORT_RANGING_TONE_TAIL)) {
		ranging->short_control.phase = V92_SHORT_RANGING_ANALOG_WAIT;
		ranging->short_control.response_samples = 0;
		ranging->short_control.tail_samples = 0;
	}
	/* Sync only opens verification. A malformed frame or late ACK must
	 * neither restart ranging nor extend the original 2500 ms deadline. */
	if (info_event->recovery_event == PCM_INFO0_RECOVERY_ACCEPTED)
		ranging->path = PCM_RANGING_SHORT_PENDING;
	if (ranging->path == PCM_RANGING_SHORT_PENDING) {
		if (!pcm_phase2_info0_exchange_complete(exchange))
			return true;
		if (!v92_short_ranging_init(&ranging->short_control, exchange->transport.signal_role,
			    ranging->actual_tone_samples, ranging->since_info0_samples))
			return false;
		ranging->path = PCM_RANGING_SHORT_ACTIVE;
		return true;
	}
	if (!v92_short_ranging_sample(&ranging->short_control,
		    pcm_phase2_info0_exchange_complete(exchange) &&
			    exchange->transport.tone_detector.carrier.tone_present,
		    &info_event->tone_event, step) ||
	    (step->reverse_carrier && v34_phase2_tone_transmitter_reverse(tone_before) !=
		    V34_PHASE2_TONE_OK) ||
	    v34_phase2_tone_transmitter_sample(tone_before, generated) != V34_PHASE2_TONE_OK)
		return false;
	exchange->transport.tone_transmitter = *tone_before;
	if (!step->transmit_tone) {
		*generated = 0;
		ranging->actual_tone_samples = 0;
	}
	if (step->event == V92_SHORT_RANGING_RECOVER_FULL_PHASE2)
		return enter_full_recovery(ranging, false);
	if (step->event != V92_SHORT_RANGING_NO_EVENT)
		ranging->path = PCM_RANGING_SHORT_FINISHED;
	return true;
}

static bool restart_after_repeated_info0(struct pcm_phase2_ranging *ranging)
{
	uint16_t prior_delay = ranging->control.prior_round_trip_delay_samples;

	/* V.90 9.2.1.2.1 and 9.2.2.2.1 restart the first-reversal exchange
	 * after repeated INFO0. Neither role may retain a reversal from before
	 * that interruption: Signal A must send a fresh initial tone and Signal
	 * B must answer its new first reversal before either can begin probing. */
	if (phase2_ranging_control_init(
		    &ranging->control,
		    &ranging->info0_exchange.transport) != PHASE2_RANGING_OK)
		return false;
	ranging->control.prior_round_trip_delay_samples = prior_delay;
	return true;
}

static enum pcm_phase2_ranging_result finish_initialization(
	struct pcm_phase2_ranging *ranging,
	struct pcm_phase2_ranging *initialized,
	enum pcm_phase2_info0_exchange_result exchange_result)
{
	if (ranging == NULL ||
	    exchange_result != PCM_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized->control,
		    &initialized->info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !pcm_phase2_ranging_is_valid(initialized))
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = *initialized;
	return PCM_PHASE2_RANGING_OK;
}

enum pcm_phase2_ranging_result pcm_phase2_ranging_init_analog(
	struct pcm_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0a)
{
	struct pcm_phase2_ranging initialized = { 0 };
	enum pcm_phase2_info0_exchange_result result;

	if (ranging == NULL)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	result = pcm_phase2_info0_exchange_init_v90_analog(
		&initialized.info0_exchange, nominal_amplitude, local_info0a);
	return finish_initialization(ranging, &initialized, result);
}

enum pcm_phase2_ranging_result pcm_phase2_ranging_init_digital(
	struct pcm_phase2_ranging *ranging,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d)
{
	struct pcm_phase2_ranging initialized = { 0 };
	enum pcm_phase2_info0_exchange_result result;

	if (ranging == NULL)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	result = pcm_phase2_info0_exchange_init_v90_digital(
		&initialized.info0_exchange, nominal_amplitude, local_info0d);
	return finish_initialization(ranging, &initialized, result);
}

enum pcm_phase2_ranging_result pcm_phase2_ranging_sample(
	struct pcm_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_ranging_event *event)
{
	struct pcm_phase2_ranging next;
	struct pcm_phase2_info0_exchange_event exchange_event;
	struct phase2_ranging_event ranging_event = { 0 };
	struct pcm_phase2_ranging_event emitted;
	struct v92_short_ranging_step short_step = { 0 };
	struct v34_phase2_tone_transmitter tone_before;
	enum phase2_info0_transmit_phase transmit_phase_before;
	int16_t generated;

	if (!pcm_phase2_ranging_is_valid(ranging) ||
	    ranging->path == PCM_RANGING_SHORT_FINISHED ||
	    transmit_sample == NULL || event == NULL)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (!uses_short_control(&next) && phase2_ranging_prepare_sample(
		    &next.control, &next.info0_exchange.transport,
		    ranging_context_is_retained(&next)) !=
	    PHASE2_RANGING_OK)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before = next.info0_exchange.transport.transmit_phase;
	tone_before = next.info0_exchange.transport.tone_transmitter;
	if (pcm_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != PCM_PHASE2_INFO0_EXCHANGE_OK)
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	count_short_timing(&next, transmit_phase_before);
	if (next.path == PCM_RANGING_FULL &&
	    next.info0_exchange.state != PCM_PHASE2_INFO0_RETRAIN &&
	    short_agreed(&next.info0_exchange)) {
		next.path = PCM_RANGING_SHORT_PENDING;
		next.short_control = (struct v92_short_ranging){ 0 };
	}
	if (uses_short_control(&next)) {
		if (!finish_short_sample(&next, &tone_before, &exchange_event,
			    &generated, &short_step))
			return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	} else {
		/* A first ACK before ranging merely completes INFO0. Once we have
		 * answered the first reversal, however, that verified record
		 * interrupts the turn: the peer's next edge is a fresh first
		 * reversal, not permission to receive L1. Restart only the ranging
		 * controller; acknowledging the ACK would create an INFO0 loop. */
		if ((exchange_event.recovery_event == PCM_INFO0_RECOVERY_ACCEPTED ||
		     (exchange_event.recovery_event == PCM_INFO0_LATE_ACK_RECEIVED &&
		      next.control.phase == PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL)) &&
		    !restart_after_repeated_info0(&next))
			return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
		if (phase2_ranging_finish_sample(
			    &next.control, &next.info0_exchange.transport,
			    ranging_context_is_retained(&next),
			    transmit_phase_before, &exchange_event.tone_event,
			    &generated, &ranging_event) != PHASE2_RANGING_OK)
			return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	}
	if (!pcm_phase2_ranging_is_valid(&next))
		return PCM_PHASE2_RANGING_INVALID_ARGUMENT;
	emitted = (struct pcm_phase2_ranging_event){
		.exchange_event = exchange_event,
		.kind = ranging_event.kind,
		.round_trip_delay_samples =
			ranging_event.round_trip_delay_samples,
		.short_step = short_step,
	};
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return PCM_PHASE2_RANGING_OK;
}
