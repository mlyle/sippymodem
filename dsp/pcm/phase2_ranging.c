// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_ranging.h"

#include <stddef.h>

static bool signal_is_b(const struct phase2_info0_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B;
}

static uint16_t completed_initial_tone_samples(
	const struct phase2_info0_exchange *exchange)
{
	return signal_is_b(exchange) ?
		       0 : PHASE2_RANGING_INITIAL_TONE_SAMPLES;
}

static bool has_ranging_context(
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received)
{
	return exchange->transmit_phase == PHASE2_INFO0_TONE &&
	       peer_info0_received;
}

static bool bounds_are_valid(
	const struct phase2_ranging_control *control)
{
	return control != NULL &&
	       (unsigned int)control->phase <= PHASE2_RANGING_TIMED_OUT &&
	       control->local_tone_samples <=
		       PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	       control->response_delay_samples <=
		       PHASE2_RANGING_TURNAROUND_SAMPLES &&
	       control->post_reversal_samples <=
		       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
	       control->probe_start_delay_samples <
		       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
	       control->measurement_elapsed_samples <=
		       PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES &&
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
	side_b = signal_is_b(exchange);
	if (control->phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    control->phase != PHASE2_RANGING_TIMED_OUT &&
	    !has_ranging_context(exchange, peer_info0_received))
		return false;

	switch (control->phase) {
	case PHASE2_RANGING_INFO0_EXCHANGE:
		return control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid &&
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
		       control->post_reversal_samples != 0 &&
		       control->post_reversal_samples <
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples ==
			       (side_b ?
					control->post_reversal_samples : 0) &&
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
	case PHASE2_RANGING_TIMED_OUT:
		return control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid &&
		       (side_b ?
				control->local_tone_samples == 0 :
				control->local_tone_samples ==
					PHASE2_RANGING_INITIAL_TONE_SAMPLES);
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
	    !has_ranging_context(exchange, peer_info0_received))
		return false;
	side_b = signal_is_b(exchange);
	if (control->phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
		return !side_b &&
		       control->local_tone_samples ==
			       PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
		       control->response_delay_samples == 0 &&
		       control->post_reversal_samples == 0 &&
		       control->probe_start_delay_samples == 0 &&
		       control->measurement_elapsed_samples == 0 &&
		       !control->round_trip_delay_valid;
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
	peer_info0_in_progress =
		exchange->info_receiver.frame.stage !=
		V34_PHASE2_INFO_SEARCH_SYNC;
	if (next.phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
	    next.local_tone_samples == PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	    !peer_info0_in_progress) {
		next.phase = PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
		reverse = true;
	} else if (next.phase == PHASE2_RESPONSE_DELAY &&
		   !peer_info0_in_progress &&
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
	    !has_ranging_context(exchange, peer_info0_received) ||
	    !exchange->tone_detector.tone_present)
		return;
	control->phase = signal_is_b(exchange) ?
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
	control->phase = PHASE2_RESPONSE_DELAY;
	return true;
}

static bool measure_round_trip_delay(
	struct phase2_ranging_control *control,
	uint16_t detector_delay_samples)
{
	uint32_t turnaround = PHASE2_RANGING_TURNAROUND_SAMPLES;
	uint32_t elapsed = control->measurement_elapsed_samples;
	uint32_t detected_edge;

	if (detector_delay_samples == 0 ||
	    elapsed < detector_delay_samples)
		return false;
	detected_edge = elapsed - detector_delay_samples;
	/* The line-terminal response is specified as 40 +/- 1 ms.  Codec and
	 * serialized duplex scheduling can date a zero-delay response on the
	 * preceding sample; accept the complete normative interval while keeping
	 * implausibly early edges on the transactional rejection path. */
	if (detected_edge +
		    PHASE2_RANGING_TURNAROUND_TOLERANCE_SAMPLES <
	    turnaround)
		return false;
	control->round_trip_delay_samples =
		detected_edge > turnaround ?
			(uint16_t)(detected_edge - turnaround) : 0u;
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
	uint16_t detector_delay_samples,
	struct phase2_ranging_event *event)
{
	struct phase2_ranging_control next = *control;
	bool accepted = false;

	switch (control->phase) {
	case PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		if (signal_is_b(exchange)) {
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
	struct phase2_ranging_event *event)
{
	if (control->phase != PHASE2_RESPONSE_TONE_TAIL)
		return;
	if (++control->post_reversal_samples <
	    PHASE2_RANGING_TONE_TAIL_SAMPLES)
		return;
	if (signal_is_b(exchange)) {
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
		signal_is_b(exchange)) ||
	       control->phase ==
		       PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL ||
	       (control->phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		!signal_is_b(exchange));
}

static void advance_measurement(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	struct phase2_ranging_event *event)
{
	if (!measurement_active(control, exchange))
		return;
	if (control->measurement_elapsed_samples <
	    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES) {
		control->measurement_elapsed_samples++;
		return;
	}
	control->phase = PHASE2_RANGING_TIMED_OUT;
	control->response_delay_samples = 0;
	control->post_reversal_samples = 0;
	control->measurement_elapsed_samples = 0;
	control->round_trip_delay_samples = 0;
	control->round_trip_delay_valid = false;
	event->kind = PHASE2_RANGING_TIMEOUT;
}

static bool transmit_phase_is_valid(
	enum phase2_info0_transmit_phase phase)
{
	return phase == PHASE2_INFO0_POST_V8_SILENCE ||
	       phase == PHASE2_INFO0_SEQUENCE ||
	       phase == PHASE2_INFO0_CARRIER_TAIL ||
	       phase == PHASE2_INFO0_TONE;
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
	struct v34_phase2_tone_detector rejected_detector;
	struct phase2_ranging_event emitted = { 0 };
	int16_t generated;
	bool reversal_rejected = false;

	if (!prepared_control_is_valid(
		    control, exchange, peer_info0_received) ||
	    !transmit_phase_is_valid(transmit_phase_before) ||
	    tone_event == NULL || transmit_sample == NULL || event == NULL ||
	    (unsigned int)tone_event->kind > V34_PHASE2_TONE_ENDED)
		return PHASE2_RANGING_INVALID_ARGUMENT;
	next = *control;
	generated = *transmit_sample;
	if (next.phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    !phase_transmits_tone(next.phase))
		generated = 0;
	/* A recovery INFO0 interrupts the continuous local-tone epoch.  The
	 * analogue modem must transmit a fresh 50 ms of Tone A after the repeated
	 * record before its first phase reversal. */
	if (!signal_is_b(exchange) &&
	    transmit_phase_before == PHASE2_INFO0_TONE &&
	    exchange->transmit_phase == PHASE2_INFO0_SEQUENCE)
		next.local_tone_samples = 0;
	else if (!signal_is_b(exchange) &&
	    next.local_tone_samples < PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	    transmit_phase_before == PHASE2_INFO0_TONE)
		next.local_tone_samples++;
	enter_active_ranging(&next, exchange, peer_info0_received);
	if (tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED &&
	    !handle_reversal(
		    &next, exchange, tone_event->reversal_delay_samples,
		    &emitted)) {
		rejected_detector = exchange->tone_detector;
		if (v34_phase2_tone_detector_reject_reversal(
			    &rejected_detector, tone_event) !=
		    V34_PHASE2_TONE_OK)
			return PHASE2_RANGING_INVALID_ARGUMENT;
		reversal_rejected = true;
	}
	finish_tone_tail(&next, exchange, &emitted);
	advance_measurement(&next, exchange, &emitted);
	advance_probe_start(&next, &emitted);
	if (!phase2_ranging_control_is_valid(
		    &next, exchange, peer_info0_received))
		return PHASE2_RANGING_INVALID_ARGUMENT;
	if (reversal_rejected)
		exchange->tone_detector = rejected_detector;
	*control = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_RANGING_OK;
}
