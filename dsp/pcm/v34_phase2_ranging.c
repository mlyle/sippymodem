// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_ranging.h"

#include <stddef.h>

static bool has_ranging_context(
	const struct v34_phase2_ranging *ranging)
{
	return ranging->info0_exchange.transport.transmit_phase ==
		       PHASE2_INFO0_TONE &&
	       ranging->info0_exchange.peer_info0_received;
}

static uint16_t completed_initial_tone_samples(
	enum v34_station_role role)
{
	return role == V34_CALLING_STATION ?
		       0 :
		       PHASE2_RANGING_INITIAL_TONE_SAMPLES;
}

bool v34_phase2_ranging_is_valid(
	const struct v34_phase2_ranging *ranging)
{
	enum v34_station_role role;

	if (ranging == NULL ||
	    !v34_phase2_info0_exchange_is_valid(
		    &ranging->info0_exchange))
		return false;
	role = ranging->info0_exchange.local_role;
	if ((unsigned int)ranging->control.phase >
		    PHASE2_RANGING_TIMED_OUT ||
	    ranging->control.local_tone_samples >
		    PHASE2_RANGING_INITIAL_TONE_SAMPLES ||
	    ranging->control.response_delay_samples >
		    PHASE2_RANGING_TURNAROUND_SAMPLES ||
	    ranging->control.post_reversal_samples >
		    PHASE2_RANGING_TONE_TAIL_SAMPLES ||
	    ranging->control.probe_start_delay_samples >=
		    PHASE2_RANGING_TONE_TAIL_SAMPLES ||
	    ranging->control.measurement_elapsed_samples >
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES ||
	    (!ranging->control.round_trip_delay_valid &&
	     ranging->control.round_trip_delay_samples != 0) ||
	    (ranging->control.round_trip_delay_valid &&
	     ranging->control.round_trip_delay_samples >
		     PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			     PHASE2_RANGING_TURNAROUND_SAMPLES))
		return false;
	if (ranging->control.phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    ranging->control.phase != PHASE2_RANGING_TIMED_OUT &&
	    !has_ranging_context(ranging))
		return false;

	switch (ranging->control.phase) {
	case PHASE2_RANGING_INFO0_EXCHANGE:
		return ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples == 0 &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples == 0 &&
		       !ranging->control.round_trip_delay_valid &&
		       (role == V34_ANSWERING_STATION ||
			ranging->control.local_tone_samples == 0);
	case PHASE2_SIGNAL_A_WAIT_INITIAL_TONE:
		return role == V34_ANSWERING_STATION &&
		       ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples == 0 &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples == 0 &&
		       !ranging->control.round_trip_delay_valid;
	case PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		return ranging->control.local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples == 0 &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       (role == V34_CALLING_STATION ?
				ranging->control.measurement_elapsed_samples == 0 :
				ranging->control.measurement_elapsed_samples != 0) &&
		       !ranging->control.round_trip_delay_valid;
	case PHASE2_RESPONSE_DELAY:
		return ranging->control.local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->control.response_delay_samples != 0 &&
		       ranging->control.post_reversal_samples == 0 &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples == 0 &&
		       ranging->control.round_trip_delay_valid ==
			       (role == V34_ANSWERING_STATION);
	case PHASE2_RESPONSE_TONE_TAIL:
		return ranging->control.local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples != 0 &&
		       ranging->control.post_reversal_samples <
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples ==
			       (role == V34_CALLING_STATION ?
					ranging->control.post_reversal_samples :
					0) &&
		       ranging->control.round_trip_delay_valid ==
			       (role == V34_ANSWERING_STATION);
	case PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL:
		return role == V34_CALLING_STATION &&
		       ranging->control.local_tone_samples == 0 &&
		       ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples ==
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples >=
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       !ranging->control.round_trip_delay_valid;
	case PHASE2_SIGNAL_B_WAIT_PROBE_START:
		return role == V34_CALLING_STATION &&
		       ranging->control.local_tone_samples == 0 &&
		       ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples ==
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->control.probe_start_delay_samples != 0 &&
		       ranging->control.measurement_elapsed_samples == 0 &&
		       ranging->control.round_trip_delay_valid;
	case PHASE2_RANGING_PROBE_READY:
		return ranging->control.local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples ==
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples == 0 &&
		       ranging->control.round_trip_delay_valid;
	case PHASE2_RANGING_TIMED_OUT:
		return ranging->control.response_delay_samples == 0 &&
		       ranging->control.post_reversal_samples == 0 &&
		       ranging->control.probe_start_delay_samples == 0 &&
		       ranging->control.measurement_elapsed_samples == 0 &&
		       !ranging->control.round_trip_delay_valid &&
		       (role == V34_CALLING_STATION ?
				ranging->control.local_tone_samples == 0 :
				ranging->control.local_tone_samples ==
					PHASE2_RANGING_INITIAL_TONE_SAMPLES);
	}
	return false;
}

enum v34_phase2_ranging_result
v34_phase2_ranging_init(
	struct v34_phase2_ranging *ranging,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0)
{
	struct v34_phase2_ranging initialized = {
		.control.phase = PHASE2_RANGING_INFO0_EXCHANGE,
	};

	if (ranging == NULL ||
	    v34_phase2_info0_exchange_init(
		    &initialized.info0_exchange, local_role,
		    nominal_amplitude, local_info0) !=
		    V34_PHASE2_INFO0_EXCHANGE_OK ||
	    !v34_phase2_ranging_is_valid(&initialized))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = initialized;
	return V34_PHASE2_RANGING_OK;
}

static bool phase_transmits_tone(
	enum phase2_ranging_phase phase)
{
	return phase ==
		       PHASE2_SIGNAL_A_WAIT_INITIAL_TONE ||
	       phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL ||
	       phase == PHASE2_RESPONSE_DELAY ||
	       phase == PHASE2_RESPONSE_TONE_TAIL;
}

static bool prepare_transmitter(
	struct v34_phase2_ranging *ranging)
{
	bool reverse = false;

	if (ranging->control.phase ==
		    PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
	    ranging->control.local_tone_samples ==
		    PHASE2_RANGING_INITIAL_TONE_SAMPLES) {
		ranging->control.phase =
			PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
		reverse = true;
	} else if (ranging->control.phase == PHASE2_RESPONSE_DELAY) {
		if (--ranging->control.response_delay_samples == 0) {
			ranging->control.phase = PHASE2_RESPONSE_TONE_TAIL;
			reverse = true;
		}
	}
	return !reverse ||
	       v34_phase2_tone_transmitter_reverse(
		       &ranging->info0_exchange.transport.tone_transmitter) ==
		       V34_PHASE2_TONE_OK;
}

static void enter_active_ranging(
	struct v34_phase2_ranging *ranging)
{
	if (ranging->control.phase !=
		    PHASE2_RANGING_INFO0_EXCHANGE ||
	    !ranging->info0_exchange.peer_info0_received ||
	    !ranging->info0_exchange.transport.tone_detector.tone_present)
		return;
	ranging->control.phase = ranging->info0_exchange.local_role ==
				 V34_CALLING_STATION ?
			 PHASE2_WAIT_FIRST_REMOTE_REVERSAL :
			 PHASE2_SIGNAL_A_WAIT_INITIAL_TONE;
}

static bool schedule_response(
	struct v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples)
{
	if (detector_delay_samples == 0 ||
	    detector_delay_samples >=
		    PHASE2_RANGING_TURNAROUND_SAMPLES)
		return false;
	ranging->control.response_delay_samples =
		PHASE2_RANGING_TURNAROUND_SAMPLES -
		detector_delay_samples;
	ranging->control.phase = PHASE2_RESPONSE_DELAY;
	return true;
}

static bool measure_round_trip_delay(
	struct v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples)
{
	uint32_t turnaround =
		PHASE2_RANGING_TURNAROUND_SAMPLES;
	uint32_t elapsed = ranging->control.measurement_elapsed_samples;

	if (detector_delay_samples == 0 ||
	    elapsed < turnaround + detector_delay_samples)
		return false;
	ranging->control.round_trip_delay_samples =
		(uint16_t)(elapsed - turnaround - detector_delay_samples);
	ranging->control.round_trip_delay_valid = true;
	ranging->control.measurement_elapsed_samples = 0;
	return true;
}

static void enter_probe_ready(
	struct v34_phase2_ranging *ranging,
	struct v34_phase2_ranging_event *event)
{
	ranging->control.phase = PHASE2_RANGING_PROBE_READY;
	ranging->control.probe_start_delay_samples = 0;
	event->kind = V34_PHASE2_PROBE_RECEIVE_READY;
	event->round_trip_delay_samples = ranging->control.round_trip_delay_samples;
}

static void schedule_probe_start(
	struct v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples,
	struct v34_phase2_ranging_event *event)
{
	if (detector_delay_samples >=
	    PHASE2_RANGING_TONE_TAIL_SAMPLES) {
		enter_probe_ready(ranging, event);
		return;
	}
	ranging->control.probe_start_delay_samples =
		PHASE2_RANGING_TONE_TAIL_SAMPLES -
		detector_delay_samples;
	ranging->control.phase = PHASE2_SIGNAL_B_WAIT_PROBE_START;
}

static void handle_reversal(
	struct v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples,
	struct v34_phase2_ranging_event *event)
{
	switch (ranging->control.phase) {
	case PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		if (ranging->info0_exchange.local_role ==
		    V34_CALLING_STATION) {
			(void)schedule_response(ranging, detector_delay_samples);
		} else if (measure_round_trip_delay(
				   ranging, detector_delay_samples) &&
			   schedule_response(ranging, detector_delay_samples)) {
			event->kind =
				V34_PHASE2_ROUND_TRIP_DELAY_MEASURED;
			event->round_trip_delay_samples =
				ranging->control.round_trip_delay_samples;
		}
		break;
	case PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL:
		if (measure_round_trip_delay(ranging, detector_delay_samples)) {
			schedule_probe_start(
				ranging, detector_delay_samples, event);
		}
		break;
	default:
		break;
	}
}

static void advance_probe_start(
	struct v34_phase2_ranging *ranging,
	struct v34_phase2_ranging_event *event)
{
	if (ranging->control.phase !=
	    PHASE2_SIGNAL_B_WAIT_PROBE_START)
		return;
	/* This call already consumed one sample after the estimated reversal
	 * edge. Emit on the final expected tail sample so the next sample can be
	 * passed directly to the probe receiver. */
	if (--ranging->control.probe_start_delay_samples == 0)
		enter_probe_ready(ranging, event);
}

static void finish_tone_tail(
	struct v34_phase2_ranging *ranging,
	struct v34_phase2_ranging_event *event)
{
	if (ranging->control.phase != PHASE2_RESPONSE_TONE_TAIL)
		return;
	if (++ranging->control.post_reversal_samples <
	    PHASE2_RANGING_TONE_TAIL_SAMPLES)
		return;
	if (ranging->info0_exchange.local_role ==
	    V34_CALLING_STATION) {
		ranging->control.phase =
			PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL;
	} else {
		ranging->control.phase = PHASE2_RANGING_PROBE_READY;
		event->kind = V34_PHASE2_PROBE_TRANSMIT_READY;
		event->round_trip_delay_samples =
			ranging->control.round_trip_delay_samples;
	}
}

static bool measurement_active(
	const struct v34_phase2_ranging *ranging)
{
	return (ranging->control.phase ==
			PHASE2_RESPONSE_TONE_TAIL &&
		ranging->info0_exchange.local_role ==
			V34_CALLING_STATION) ||
	       ranging->control.phase ==
		       PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL ||
	       (ranging->control.phase ==
			PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		ranging->info0_exchange.local_role ==
			V34_ANSWERING_STATION);
}

static void advance_measurement(
	struct v34_phase2_ranging *ranging,
	struct v34_phase2_ranging_event *event)
{
	if (!measurement_active(ranging))
		return;
	if (ranging->control.measurement_elapsed_samples <
	    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES) {
		ranging->control.measurement_elapsed_samples++;
		return;
	}
	ranging->control.phase = PHASE2_RANGING_TIMED_OUT;
	ranging->control.response_delay_samples = 0;
	ranging->control.post_reversal_samples = 0;
	ranging->control.measurement_elapsed_samples = 0;
	ranging->control.round_trip_delay_samples = 0;
	ranging->control.round_trip_delay_valid = false;
	event->kind = V34_PHASE2_RANGING_TIMEOUT;
}

enum v34_phase2_ranging_result
v34_phase2_ranging_sample(
	struct v34_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_ranging_event *event)
{
	struct v34_phase2_ranging next;
	struct v34_phase2_ranging_event emitted = { 0 };
	enum phase2_info0_transmit_phase transmit_phase_before;
	bool pure_tone_sample;
	int16_t generated;

	if (!v34_phase2_ranging_is_valid(ranging) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (!prepare_transmitter(&next))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before =
		next.info0_exchange.transport.transmit_phase;
	pure_tone_sample = transmit_phase_before ==
			   PHASE2_INFO0_TONE;
	if (v34_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &emitted.exchange_event) !=
	    V34_PHASE2_INFO0_EXCHANGE_OK)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	if (next.control.phase != PHASE2_RANGING_INFO0_EXCHANGE &&
	    !phase_transmits_tone(next.control.phase))
		generated = 0;
	if (next.info0_exchange.local_role ==
		    V34_ANSWERING_STATION &&
	    next.control.local_tone_samples <
		    PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	    pure_tone_sample)
		next.control.local_tone_samples++;
	enter_active_ranging(&next);
	if (emitted.exchange_event.tone_event.kind ==
	    V34_PHASE2_TONE_PHASE_REVERSED)
		handle_reversal(
			&next,
			emitted.exchange_event.tone_event.reversal_delay_samples,
			&emitted);
	finish_tone_tail(&next, &emitted);
	advance_measurement(&next, &emitted);
	advance_probe_start(&next, &emitted);
	if (!v34_phase2_ranging_is_valid(&next))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_RANGING_OK;
}

const char *v34_phase2_ranging_result_name(
	enum v34_phase2_ranging_result result)
{
	switch (result) {
	case V34_PHASE2_RANGING_OK:
		return "ok";
	case V34_PHASE2_RANGING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
