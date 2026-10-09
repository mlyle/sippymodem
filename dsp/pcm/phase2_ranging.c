// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_ranging.h"

#include <stddef.h>

static uint16_t completed_initial_tone_samples(
	const struct phase2_info0_exchange *exchange)
{
	return phase2_info0_signal_is_b(exchange) ?
		       0 : PHASE2_RANGING_INITIAL_TONE_SAMPLES;
}

static bool retains_ranging_context(
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received)
{
	return exchange->transmit_phase != PHASE2_INFO0_POST_V8_SILENCE &&
	       peer_info0_received;
}

static bool bounds_are_valid(
	const struct phase2_ranging_control *control)
{
	return control != NULL &&
	       (unsigned int)control->phase <= PHASE2_RANGING_PROBE_READY &&
	       control->local_tone_samples <=
		       PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	       control->deferred_reversal_samples <
		       PHASE2_RANGING_TURNAROUND_SAMPLES &&
	       control->response_delay_samples <=
		       PHASE2_RANGING_TURNAROUND_SAMPLES &&
	       control->post_reversal_samples <=
		       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
	       control->probe_start_delay_samples <
		       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
	       control->probe_reversal_elapsed_samples <=
		       PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES &&
	       control->measurement_elapsed_samples <=
		       PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES &&
	       control->prior_round_trip_delay_samples <=
		       PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			       PHASE2_RANGING_TURNAROUND_SAMPLES &&
	       (control->round_trip_delay_valid ||
		control->round_trip_delay_samples == 0) &&
	       (!control->round_trip_delay_valid ||
		control->round_trip_delay_samples <=
			PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
				PHASE2_RANGING_TURNAROUND_SAMPLES);
}

bool phase2_ranging_control_is_valid(
	const struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received)
{
	bool side_b;

	if (!bounds_are_valid(control) ||
	    !phase2_info0_exchange_is_valid(exchange))
		return false;
	side_b = phase2_info0_signal_is_b(exchange);
	if ((control->probe_reversal_elapsed_samples != 0) !=
	    (side_b && (control->phase == PHASE2_SIGNAL_B_WAIT_PROBE_START ||
			control->phase == PHASE2_RANGING_PROBE_READY)))
		return false;
	if (control->phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    control->deferred_reversal_samples != 0)
		return false;
	if (control->phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    !retains_ranging_context(exchange, peer_info0_received))
		return false;

	switch (control->phase) {
	case PHASE2_RANGING_INFO0_EXCHANGE:
		return control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid &&
		       (control->deferred_reversal_samples == 0 ||
			(side_b &&
			 exchange->transmit_phase !=
				 PHASE2_INFO0_POST_V8_SILENCE)) &&
		       (!side_b || control->local_tone_samples == 0);
	case PHASE2_SIGNAL_A_WAIT_INITIAL_TONE:
		return !side_b && control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid;
	case PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		return control->local_tone_samples ==
			       completed_initial_tone_samples(exchange) &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       (side_b ?
				control->measurement_elapsed_samples == 0 :
				control->measurement_elapsed_samples != 0) &&
		       !control->round_trip_delay_valid;
	case PHASE2_RESPONSE_DELAY:
		return control->local_tone_samples ==
			       completed_initial_tone_samples(exchange) &&
		       control->response_delay_samples != 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       control->round_trip_delay_valid == !side_b;
	case PHASE2_RESPONSE_TONE_TAIL:
		return control->local_tone_samples ==
			       completed_initial_tone_samples(exchange) &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples <
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       control->probe_start_delay_samples == 0 &&
		       (side_b ?
				control->measurement_elapsed_samples >=
					control->post_reversal_samples :
				control->measurement_elapsed_samples == 0) &&
		       control->round_trip_delay_valid == !side_b;
	case PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL:
		return side_b && control->local_tone_samples == 0 &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples ==
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples >=
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       !control->round_trip_delay_valid;
	case PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE:
		return side_b && control->local_tone_samples == 0 &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid;
	case PHASE2_SIGNAL_B_WAIT_PROBE_START:
		return side_b && control->local_tone_samples == 0 &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples ==
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       control->probe_start_delay_samples != 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       control->round_trip_delay_valid;
	case PHASE2_RANGING_PROBE_READY:
		return control->local_tone_samples ==
			       completed_initial_tone_samples(exchange) &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples ==
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       control->round_trip_delay_valid;
	}
	return false;
}

enum phase2_ranging_result phase2_ranging_control_init(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange)
{
	struct phase2_ranging_control initialized = {
		.phase = PHASE2_RANGING_INFO0_EXCHANGE,
	};

	if (control == NULL ||
	    !phase2_ranging_control_is_valid(
		    &initialized, exchange, false))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	*control = initialized;
	return PHASE2_RANGING_OK;
}

enum phase2_ranging_result phase2_ranging_restart_after_probe(
	struct phase2_ranging_control *control,
	struct phase2_info0_exchange *exchange)
{
	struct phase2_ranging_control next = {
		.phase = PHASE2_SIGNAL_A_WAIT_INITIAL_TONE,
	};
	struct phase2_info0_exchange transport;

	if (!phase2_ranging_control_is_valid(control, exchange, true) ||
	    control->phase != PHASE2_RANGING_PROBE_READY ||
	    exchange->signal_role != PHASE2_SIGNAL_A ||
	    exchange->transmit_phase != PHASE2_INFO0_TONE)
		return PHASE2_RANGING_INVALID_ARGUMENT;
	transport = *exchange;
	/* An unanswered probe does not confirm its ranging estimate. Preserve
	 * only a delay supplied by an established call, so this new epoch can
	 * correct a false first measurement instead of treating it as an alias. */
	next.prior_round_trip_delay_samples = control->prior_round_trip_delay_samples;
	/* The INFO receiver was idle during probing. Discard its old sample
	 * history together with the Tone-B detector's previous reversal epoch. */
	if (v34_phase2_tone_detector_reset(&transport.tone_detector) !=
		    V34_PHASE2_TONE_OK ||
	    v34_phase2_info_audio_receiver_init(
		    &transport.info_receiver,
		    v34_phase2_station_for_signal(transport.signal_role),
		    transport.info_receiver.frame.expected_payload_bit_count) !=
		    V34_PHASE2_INFO_AUDIO_OK ||
	    !phase2_ranging_control_is_valid(&next, &transport, true))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	*control = next;
	*exchange = transport;
	return PHASE2_RANGING_OK;
}

static bool prepared_control_is_valid(
	const struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received)
{
	bool side_b;

	if (phase2_ranging_control_is_valid(
		    control, exchange, peer_info0_received))
		return true;
	if (!bounds_are_valid(control) ||
	    !phase2_info0_exchange_is_valid(exchange) ||
	    !retains_ranging_context(exchange, peer_info0_received))
		return false;
	side_b = phase2_info0_signal_is_b(exchange);
	if (control->phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
		return !side_b &&
		       control->local_tone_samples ==
			       PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid;
	if (control->phase == PHASE2_RANGING_INFO0_EXCHANGE && side_b &&
	    control->deferred_reversal_samples != 0 &&
	    control->response_delay_samples == 0 &&
	    control->post_reversal_samples == 0 &&
	    control->probe_start_delay_samples == 0 &&
	    control->measurement_elapsed_samples == 0 &&
	    !control->round_trip_delay_valid &&
	    exchange->transmit_phase != PHASE2_INFO0_POST_V8_SILENCE)
		return true;
	return control->phase == PHASE2_RESPONSE_TONE_TAIL &&
	       control->local_tone_samples ==
		       completed_initial_tone_samples(exchange) &&
	       control->response_delay_samples == 0 &&
	       control->post_reversal_samples == 0 &&
	       control->probe_start_delay_samples == 0 &&
	       control->measurement_elapsed_samples == 0 &&
	       control->round_trip_delay_valid == !side_b;
}

enum phase2_ranging_result phase2_ranging_prepare_sample(
	struct phase2_ranging_control *control,
	struct phase2_info0_exchange *exchange,
	bool peer_info0_received)
{
	struct phase2_ranging_control next;
	bool peer_info0_in_progress;
	bool reverse = false;

	if (!phase2_ranging_control_is_valid(
		    control, exchange, peer_info0_received))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	next = *control;
	if (next.deferred_reversal_samples != 0 &&
	    ++next.deferred_reversal_samples ==
		    PHASE2_RANGING_TURNAROUND_SAMPLES)
		next.deferred_reversal_samples = 0;
	peer_info0_in_progress =
		exchange->info_receiver.frame.stage !=
		V34_PHASE2_INFO_SEARCH_SYNC;
	if (next.phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
	    next.local_tone_samples == PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	    exchange->tone_detector.carrier.tone_present &&
	    !peer_info0_in_progress) {
		next.phase = PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
		reverse = true;
	} else if (next.phase == PHASE2_RESPONSE_DELAY &&
		   --next.response_delay_samples == 0) {
		next.phase = PHASE2_RESPONSE_TONE_TAIL;
		reverse = true;
	}
	if (reverse) {
		struct phase2_info0_exchange transport = *exchange;

		if (v34_phase2_tone_transmitter_reverse(
			    &transport.tone_transmitter) !=
			    V34_PHASE2_TONE_OK ||
		    !prepared_control_is_valid(
			    &next, &transport, peer_info0_received))
			return PHASE2_RANGING_INVALID_ARGUMENT;
		*exchange = transport;
	} else if (!prepared_control_is_valid(
			   &next, exchange, peer_info0_received)) {
		return PHASE2_RANGING_INVALID_ARGUMENT;
	}
	*control = next;
	return PHASE2_RANGING_OK;
}

static bool phase_transmits_tone(enum phase2_ranging_phase phase)
{
	return phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE ||
	       phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL ||
	       phase == PHASE2_RESPONSE_DELAY ||
	       phase == PHASE2_RESPONSE_TONE_TAIL;
}

static void enter_active_ranging(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received)
{
	if (control->phase != PHASE2_RANGING_INFO0_EXCHANGE ||
	    exchange->transmit_phase != PHASE2_INFO0_TONE ||
	    !retains_ranging_context(exchange, peer_info0_received) ||
	    !exchange->tone_detector.carrier.tone_present)
		return;
	control->phase = phase2_info0_signal_is_b(exchange) ?
				 PHASE2_WAIT_FIRST_REMOTE_REVERSAL :
				 PHASE2_SIGNAL_A_WAIT_INITIAL_TONE;
}

static bool schedule_response(
	struct phase2_ranging_control *control,
	uint16_t detector_delay_samples)
{
	if (detector_delay_samples == 0 ||
	    detector_delay_samples >= PHASE2_RANGING_TURNAROUND_SAMPLES)
		return false;
	control->response_delay_samples =
		PHASE2_RANGING_TURNAROUND_SAMPLES - detector_delay_samples;
	control->deferred_reversal_samples = 0;
	control->phase = PHASE2_RESPONSE_DELAY;
	return true;
}

static bool defer_response(
	struct phase2_ranging_control *control,
	uint16_t detector_delay_samples)
{
	if (control->deferred_reversal_samples != 0 ||
	    detector_delay_samples == 0 ||
	    detector_delay_samples >= PHASE2_RANGING_TURNAROUND_SAMPLES)
		return false;
	control->deferred_reversal_samples = detector_delay_samples;
	return true;
}

static bool resume_deferred_response(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange)
{
	uint16_t elapsed = control->deferred_reversal_samples;

	if (elapsed == 0)
		return true;
	if (control->phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
	    phase2_info0_signal_is_b(exchange))
		return schedule_response(control, elapsed);
	if (control->phase == PHASE2_RANGING_INFO0_EXCHANGE &&
	    exchange->transmit_phase != PHASE2_INFO0_TONE)
		return true;
	control->deferred_reversal_samples = 0;
	return true;
}

static bool measure_round_trip_delay(
	struct phase2_ranging_control *control,
	uint16_t detector_delay_samples)
{
	uint32_t turnaround = PHASE2_RANGING_TURNAROUND_SAMPLES;
	uint32_t elapsed = control->measurement_elapsed_samples;
	uint32_t detected_edge;
	uint32_t measured;
	uint32_t prior = control->prior_round_trip_delay_samples;
	uint32_t separation;

	if (detector_delay_samples == 0 ||
	    elapsed < detector_delay_samples)
		return false;
	detected_edge = elapsed - detector_delay_samples;
	/* The line-terminal response is specified as 40 +/- 1 ms. Codec and
	 * serialized duplex scheduling can date a zero-delay response on the
	 * preceding sample; accept the complete normative interval while keeping
	 * implausibly early edges on the transactional rejection path. During a
	 * full retrain, however, the already-confirmed propagation delay makes a
	 * qualified reversal within the preceding 10 ms tone tail unambiguous.
	 * Accept that interoperability window, but retain the confirmed delay. */
	if (detected_edge +
		    PHASE2_RANGING_TURNAROUND_TOLERANCE_SAMPLES <
	    turnaround &&
	    (prior == 0u ||
	     detected_edge + PHASE2_RANGING_TONE_TAIL_SAMPLES < turnaround))
		return false;
	measured = detected_edge > turnaround ? detected_edge - turnaround : 0u;
	separation = measured > prior ? measured - prior : prior - measured;
	if (prior != 0u && separation > turnaround)
		measured = prior;
	control->round_trip_delay_samples = (uint16_t)measured;
	control->round_trip_delay_valid = true;
	control->measurement_elapsed_samples = 0;
	return true;
}

static void enter_probe_ready(
	struct phase2_ranging_control *control,
	struct phase2_ranging_event *event)
{
	control->phase = PHASE2_RANGING_PROBE_READY;
	control->probe_start_delay_samples = 0;
	event->kind = PHASE2_PROBE_RECEIVE_READY;
	event->round_trip_delay_samples = control->round_trip_delay_samples;
}

static void schedule_probe_start(
	struct phase2_ranging_control *control,
	uint16_t detector_delay_samples,
	struct phase2_ranging_event *event)
{
	control->probe_reversal_elapsed_samples = detector_delay_samples;
	if (detector_delay_samples >= PHASE2_RANGING_TONE_TAIL_SAMPLES) {
		enter_probe_ready(control, event);
		return;
	}
	control->probe_start_delay_samples =
		PHASE2_RANGING_TONE_TAIL_SAMPLES - detector_delay_samples;
	control->phase = PHASE2_SIGNAL_B_WAIT_PROBE_START;
}

static bool handle_reversal(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received,
	enum phase2_info0_transmit_phase transmit_phase_before,
	uint16_t detector_delay_samples,
	struct phase2_ranging_event *event)
{
	struct phase2_ranging_control next = *control;
	bool accepted = false;

	switch (control->phase) {
	case PHASE2_RANGING_INFO0_EXCHANGE:
		if (phase2_info0_signal_is_b(exchange) && peer_info0_received &&
		    transmit_phase_before != PHASE2_INFO0_POST_V8_SILENCE &&
		    exchange->transmit_phase != PHASE2_INFO0_POST_V8_SILENCE) {
			accepted = defer_response(
				&next, detector_delay_samples);
		}
		break;
	case PHASE2_SIGNAL_A_WAIT_INITIAL_TONE:
		/* A peer edge delivered on the final 50 ms Tone A sample cannot
		 * answer the local reversal, which is prepared on the next call.
		 * Retain its following-tone phase as the detector baseline. */
		if (!phase2_info0_signal_is_b(exchange) &&
		    control->local_tone_samples ==
			    PHASE2_RANGING_INITIAL_TONE_SAMPLES)
			accepted = true;
		break;
	case PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		if (phase2_info0_signal_is_b(exchange)) {
			accepted = schedule_response(
				&next, detector_delay_samples);
		} else if (measure_round_trip_delay(
				   &next, detector_delay_samples) &&
			   schedule_response(&next, detector_delay_samples)) {
			event->kind = PHASE2_ROUND_TRIP_DELAY_MEASURED;
			event->round_trip_delay_samples =
				next.round_trip_delay_samples;
			accepted = true;
		}
		break;
	case PHASE2_RESPONSE_DELAY:
		/* A second peer reversal before the scheduled local response
		 * proves that the first edge did not begin a valid ranging turn.
		 * Keep the detector's newly restored phase as the reference and
		 * wait for a fresh first edge. Rejecting this event would restore
		 * the false intermediate phase. */
		if (phase2_info0_signal_is_b(exchange)) {
			next = (struct phase2_ranging_control){
				.phase = PHASE2_WAIT_FIRST_REMOTE_REVERSAL,
			};
			accepted = true;
		}
		break;
	case PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL:
		if (measure_round_trip_delay(&next, detector_delay_samples)) {
			schedule_probe_start(
				&next, detector_delay_samples, event);
			accepted = true;
		}
		break;
	default:
		break;
	}
	if (accepted)
		*control = next;
	return accepted;
}

static void advance_probe_start(
	struct phase2_ranging_control *control,
	struct phase2_ranging_event *event)
{
	if (control->phase != PHASE2_SIGNAL_B_WAIT_PROBE_START)
		return;
	/* This call already consumed one sample after the estimated reversal
	 * edge. Emit on the final expected tail sample so the next sample can be
	 * passed directly to the probe receiver. */
	if (--control->probe_start_delay_samples == 0)
		enter_probe_ready(control, event);
}

static void finish_tone_tail(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	enum phase2_info0_transmit_phase transmit_phase_before,
	struct phase2_ranging_event *event)
{
	if (control->phase != PHASE2_RESPONSE_TONE_TAIL ||
	    transmit_phase_before != PHASE2_INFO0_TONE)
		return;
	if (++control->post_reversal_samples <
	    PHASE2_RANGING_TONE_TAIL_SAMPLES)
		return;
	if (phase2_info0_signal_is_b(exchange)) {
		control->phase = PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL;
	} else {
		control->phase = PHASE2_RANGING_PROBE_READY;
		event->kind = PHASE2_PROBE_TRANSMIT_READY;
		event->round_trip_delay_samples =
			control->round_trip_delay_samples;
	}
}

static bool measurement_active(
	const struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange)
{
	return (control->phase == PHASE2_RESPONSE_TONE_TAIL &&
		phase2_info0_signal_is_b(exchange)) ||
	       control->phase ==
		       PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL ||
	       (control->phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		!phase2_info0_signal_is_b(exchange));
}

static void recover_signal_b_after_remote_tone(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange)
{
	if (control->phase ==
		    PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE &&
	    exchange->tone_detector.carrier.tone_present)
		control->phase = PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
}

static bool advance_measurement(
	struct phase2_ranging_control *control,
	struct phase2_info0_exchange *exchange)
{
	if (!measurement_active(control, exchange))
		return true;
	if (control->measurement_elapsed_samples <
	    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES) {
		control->measurement_elapsed_samples++;
		return true;
	}
	control->response_delay_samples = 0;
	control->post_reversal_samples = 0;
	control->probe_start_delay_samples = 0;
	control->measurement_elapsed_samples = 0;
	control->round_trip_delay_samples = 0;
	control->round_trip_delay_valid = false;
	/* Signal B has already supplied nearly two seconds of silence while
	 * waiting for the second remote reversal.  The shared V.34/PCM recovery
	 * is to reacquire Tone A, resume Tone B, and begin a fresh reversal
	 * exchange.  The owning training-session deadline remains the hard bound
	 * on repeated attempts. */
	if (phase2_info0_signal_is_b(exchange)) {
		if (exchange->tone_detector.carrier.tone_present) {
			control->phase = PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
			return true;
		}
		/* Loss normally retains the old carrier phase so INFO interruptions
		 * can classify their following edge.  This is a new ranging epoch:
		 * reacquired Tone A establishes its own reference regardless of which
		 * phase the peer resumes. */
		if (v34_phase2_tone_detector_reset(
			    &exchange->tone_detector) != V34_PHASE2_TONE_OK)
			return false;
		control->phase = PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE;
		return true;
	}
	/* V.34 11.2.2.2.2 / V.90 9.2.2.2.2: an unanswered first reversal
	 * restarts Tone-B acquisition, not the call. Tone A has remained on;
	 * retain its completed guard but qualify a fresh remote phase before
	 * sending another reversal. The training-session deadline still bounds
	 * a peer that never responds. */
	if (v34_phase2_tone_detector_reset(
		    &exchange->tone_detector) != V34_PHASE2_TONE_OK)
		return false;
	control->phase = PHASE2_SIGNAL_A_WAIT_INITIAL_TONE;
	return true;
}

enum phase2_ranging_result phase2_ranging_finish_sample(
	struct phase2_ranging_control *control,
	struct phase2_info0_exchange *exchange,
	bool peer_info0_received,
	enum phase2_info0_transmit_phase transmit_phase_before,
	const struct v34_phase2_tone_event *tone_event,
	int16_t *transmit_sample, struct phase2_ranging_event *event)
{
	struct phase2_ranging_control next;
	struct phase2_ranging_event emitted = { 0 };
	int16_t generated;

	if (!prepared_control_is_valid(
		    control, exchange, peer_info0_received) ||
	    !phase2_info0_transmit_phase_is_valid(transmit_phase_before) ||
	    tone_event == NULL || transmit_sample == NULL || event == NULL ||
	    (unsigned int)tone_event->kind > V34_PHASE2_TONE_ENDED)
		return PHASE2_RANGING_INVALID_ARGUMENT;
	next = *control;
	generated = *transmit_sample;
	if (next.phase == PHASE2_SIGNAL_B_WAIT_PROBE_START)
		next.probe_reversal_elapsed_samples++;
	if (exchange->transmit_phase == PHASE2_INFO0_TONE &&
	    next.phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    !phase_transmits_tone(next.phase))
		generated = 0;
	/* A recovery INFO0 interrupts the continuous local-tone epoch.  The
	 * analogue modem must transmit a fresh 50 ms of Tone A after the repeated
	 * record before its first phase reversal. */
	if (!phase2_info0_signal_is_b(exchange) &&
	    (next.phase == PHASE2_RANGING_INFO0_EXCHANGE ||
	     next.phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE) &&
	    transmit_phase_before == PHASE2_INFO0_TONE &&
	    exchange->transmit_phase == PHASE2_INFO0_SEQUENCE)
		next.local_tone_samples = 0;
	else if (!phase2_info0_signal_is_b(exchange) &&
	    next.local_tone_samples < PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	    transmit_phase_before == PHASE2_INFO0_TONE)
		next.local_tone_samples++;
	enter_active_ranging(&next, exchange, peer_info0_received);
	if (!resume_deferred_response(&next, exchange))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	if (tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED &&
	    !handle_reversal(
		    &next, exchange, peer_info0_received, transmit_phase_before,
		    tone_event->reversal_delay_samples,
		    &emitted)) {
		/* The owner supplies a candidate transport. Reject this edge before
		 * timeout recovery can establish a fresh detector epoch below. */
		if (v34_phase2_tone_detector_reject_reversal(
			    &exchange->tone_detector, tone_event) !=
		    V34_PHASE2_TONE_OK)
			return PHASE2_RANGING_INVALID_ARGUMENT;
	}
	finish_tone_tail(
		&next, exchange, transmit_phase_before, &emitted);
	if (!advance_measurement(&next, exchange))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	recover_signal_b_after_remote_tone(&next, exchange);
	advance_probe_start(&next, &emitted);
	if (!phase2_ranging_control_is_valid(
		    &next, exchange, peer_info0_received))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	*control = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_RANGING_OK;
}
