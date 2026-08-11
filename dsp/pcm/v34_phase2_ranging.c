// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_ranging.h"

#include <stddef.h>

static bool has_ranging_context(
	const struct sippymodem_v34_phase2_ranging *ranging)
{
	return ranging->info0_exchange.transmit_phase ==
		       SIPPYMODEM_V34_PHASE2_INFO0_TONE &&
	       ranging->info0_exchange.peer_info0_received;
}

static uint16_t completed_initial_tone_samples(
	enum sippymodem_v34_station_role role)
{
	return role == SIPPYMODEM_V34_CALLING_STATION ?
		       0 :
		       SIPPYMODEM_V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES;
}

bool sippymodem_v34_phase2_ranging_is_valid(
	const struct sippymodem_v34_phase2_ranging *ranging)
{
	enum sippymodem_v34_station_role role;

	if (ranging == NULL ||
	    !sippymodem_v34_phase2_info0_exchange_is_valid(
		    &ranging->info0_exchange))
		return false;
	role = ranging->info0_exchange.local_role;
	if ((unsigned int)ranging->phase >
		    SIPPYMODEM_V34_PHASE2_RANGING_TIMED_OUT ||
	    ranging->local_tone_samples >
		    SIPPYMODEM_V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES ||
	    ranging->response_delay_samples >
		    SIPPYMODEM_V34_PHASE2_RANGING_TURNAROUND_SAMPLES ||
	    ranging->post_reversal_samples >
		    SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES ||
	    ranging->probe_start_delay_samples >=
		    SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES ||
	    ranging->measurement_elapsed_samples >
		    SIPPYMODEM_V34_PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES ||
	    (!ranging->round_trip_delay_valid &&
	     ranging->round_trip_delay_samples != 0) ||
	    (ranging->round_trip_delay_valid &&
	     ranging->round_trip_delay_samples >
		     SIPPYMODEM_V34_PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			     SIPPYMODEM_V34_PHASE2_RANGING_TURNAROUND_SAMPLES))
		return false;
	if (ranging->phase != SIPPYMODEM_V34_PHASE2_RANGING_INFO0_EXCHANGE &&
	    ranging->phase != SIPPYMODEM_V34_PHASE2_RANGING_TIMED_OUT &&
	    !has_ranging_context(ranging))
		return false;

	switch (ranging->phase) {
	case SIPPYMODEM_V34_PHASE2_RANGING_INFO0_EXCHANGE:
		return ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples == 0 &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples == 0 &&
		       !ranging->round_trip_delay_valid &&
		       (role == SIPPYMODEM_V34_ANSWERING_STATION ||
			ranging->local_tone_samples == 0);
	case SIPPYMODEM_V34_PHASE2_ANSWERER_WAIT_INITIAL_TONE:
		return role == SIPPYMODEM_V34_ANSWERING_STATION &&
		       ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples == 0 &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples == 0 &&
		       !ranging->round_trip_delay_valid;
	case SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		return ranging->local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples == 0 &&
		       ranging->probe_start_delay_samples == 0 &&
		       (role == SIPPYMODEM_V34_CALLING_STATION ?
				ranging->measurement_elapsed_samples == 0 :
				ranging->measurement_elapsed_samples != 0) &&
		       !ranging->round_trip_delay_valid;
	case SIPPYMODEM_V34_PHASE2_RESPONSE_DELAY:
		return ranging->local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->response_delay_samples != 0 &&
		       ranging->post_reversal_samples == 0 &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples == 0 &&
		       ranging->round_trip_delay_valid ==
			       (role == SIPPYMODEM_V34_ANSWERING_STATION);
	case SIPPYMODEM_V34_PHASE2_RESPONSE_TONE_TAIL:
		return ranging->local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples != 0 &&
		       ranging->post_reversal_samples <
			       SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples ==
			       (role == SIPPYMODEM_V34_CALLING_STATION ?
					ranging->post_reversal_samples :
					0) &&
		       ranging->round_trip_delay_valid ==
			       (role == SIPPYMODEM_V34_ANSWERING_STATION);
	case SIPPYMODEM_V34_PHASE2_CALLER_WAIT_SECOND_REMOTE_REVERSAL:
		return role == SIPPYMODEM_V34_CALLING_STATION &&
		       ranging->local_tone_samples == 0 &&
		       ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples ==
			       SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples >=
			       SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       !ranging->round_trip_delay_valid;
	case SIPPYMODEM_V34_PHASE2_CALLER_WAIT_PROBE_START:
		return role == SIPPYMODEM_V34_CALLING_STATION &&
		       ranging->local_tone_samples == 0 &&
		       ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples ==
			       SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->probe_start_delay_samples != 0 &&
		       ranging->measurement_elapsed_samples == 0 &&
		       ranging->round_trip_delay_valid;
	case SIPPYMODEM_V34_PHASE2_PROBE_READY:
		return ranging->local_tone_samples ==
			       completed_initial_tone_samples(role) &&
		       ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples ==
			       SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples == 0 &&
		       ranging->round_trip_delay_valid;
	case SIPPYMODEM_V34_PHASE2_RANGING_TIMED_OUT:
		return ranging->response_delay_samples == 0 &&
		       ranging->post_reversal_samples == 0 &&
		       ranging->probe_start_delay_samples == 0 &&
		       ranging->measurement_elapsed_samples == 0 &&
		       !ranging->round_trip_delay_valid &&
		       (role == SIPPYMODEM_V34_CALLING_STATION ?
				ranging->local_tone_samples == 0 :
				ranging->local_tone_samples ==
					SIPPYMODEM_V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES);
	}
	return false;
}

enum sippymodem_v34_phase2_ranging_result
sippymodem_v34_phase2_ranging_init(
	struct sippymodem_v34_phase2_ranging *ranging,
	enum sippymodem_v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct sippymodem_v34_info0 *local_info0)
{
	struct sippymodem_v34_phase2_ranging initialized = {
		.phase = SIPPYMODEM_V34_PHASE2_RANGING_INFO0_EXCHANGE,
	};

	if (ranging == NULL ||
	    sippymodem_v34_phase2_info0_exchange_init(
		    &initialized.info0_exchange, local_role,
		    nominal_amplitude, local_info0) !=
		    SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_OK ||
	    !sippymodem_v34_phase2_ranging_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = initialized;
	return SIPPYMODEM_V34_PHASE2_RANGING_OK;
}

static bool phase_transmits_tone(
	enum sippymodem_v34_phase2_ranging_phase phase)
{
	return phase ==
		       SIPPYMODEM_V34_PHASE2_ANSWERER_WAIT_INITIAL_TONE ||
	       phase == SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL ||
	       phase == SIPPYMODEM_V34_PHASE2_RESPONSE_DELAY ||
	       phase == SIPPYMODEM_V34_PHASE2_RESPONSE_TONE_TAIL;
}

static bool prepare_transmitter(
	struct sippymodem_v34_phase2_ranging *ranging)
{
	bool reverse = false;

	if (ranging->phase ==
		    SIPPYMODEM_V34_PHASE2_ANSWERER_WAIT_INITIAL_TONE &&
	    ranging->local_tone_samples ==
		    SIPPYMODEM_V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES) {
		ranging->phase =
			SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
		reverse = true;
	} else if (ranging->phase == SIPPYMODEM_V34_PHASE2_RESPONSE_DELAY) {
		if (--ranging->response_delay_samples == 0) {
			ranging->phase = SIPPYMODEM_V34_PHASE2_RESPONSE_TONE_TAIL;
			reverse = true;
		}
	}
	return !reverse ||
	       sippymodem_v34_phase2_tone_transmitter_reverse(
		       &ranging->info0_exchange.tone_transmitter) ==
		       SIPPYMODEM_V34_PHASE2_TONE_OK;
}

static void enter_active_ranging(
	struct sippymodem_v34_phase2_ranging *ranging)
{
	if (ranging->phase !=
		    SIPPYMODEM_V34_PHASE2_RANGING_INFO0_EXCHANGE ||
	    !ranging->info0_exchange.peer_info0_received ||
	    !ranging->info0_exchange.tone_detector.tone_present)
		return;
	ranging->phase = ranging->info0_exchange.local_role ==
				 SIPPYMODEM_V34_CALLING_STATION ?
			 SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL :
			 SIPPYMODEM_V34_PHASE2_ANSWERER_WAIT_INITIAL_TONE;
}

static bool schedule_response(
	struct sippymodem_v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples)
{
	if (detector_delay_samples == 0 ||
	    detector_delay_samples >=
		    SIPPYMODEM_V34_PHASE2_RANGING_TURNAROUND_SAMPLES)
		return false;
	ranging->response_delay_samples =
		SIPPYMODEM_V34_PHASE2_RANGING_TURNAROUND_SAMPLES -
		detector_delay_samples;
	ranging->phase = SIPPYMODEM_V34_PHASE2_RESPONSE_DELAY;
	return true;
}

static bool measure_round_trip_delay(
	struct sippymodem_v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples)
{
	uint32_t turnaround =
		SIPPYMODEM_V34_PHASE2_RANGING_TURNAROUND_SAMPLES;
	uint32_t elapsed = ranging->measurement_elapsed_samples;

	if (detector_delay_samples == 0 ||
	    elapsed < turnaround + detector_delay_samples)
		return false;
	ranging->round_trip_delay_samples =
		(uint16_t)(elapsed - turnaround - detector_delay_samples);
	ranging->round_trip_delay_valid = true;
	ranging->measurement_elapsed_samples = 0;
	return true;
}

static void enter_probe_ready(
	struct sippymodem_v34_phase2_ranging *ranging,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	ranging->phase = SIPPYMODEM_V34_PHASE2_PROBE_READY;
	ranging->probe_start_delay_samples = 0;
	event->kind = SIPPYMODEM_V34_PHASE2_PROBE_RECEIVE_READY;
	event->round_trip_delay_samples = ranging->round_trip_delay_samples;
}

static void schedule_probe_start(
	struct sippymodem_v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	if (detector_delay_samples >=
	    SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES) {
		enter_probe_ready(ranging, event);
		return;
	}
	ranging->probe_start_delay_samples =
		SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES -
		detector_delay_samples;
	ranging->phase = SIPPYMODEM_V34_PHASE2_CALLER_WAIT_PROBE_START;
}

static void handle_reversal(
	struct sippymodem_v34_phase2_ranging *ranging,
	uint16_t detector_delay_samples,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	switch (ranging->phase) {
	case SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL:
		if (ranging->info0_exchange.local_role ==
		    SIPPYMODEM_V34_CALLING_STATION) {
			(void)schedule_response(ranging, detector_delay_samples);
		} else if (measure_round_trip_delay(
				   ranging, detector_delay_samples) &&
			   schedule_response(ranging, detector_delay_samples)) {
			event->kind =
				SIPPYMODEM_V34_PHASE2_ROUND_TRIP_DELAY_MEASURED;
			event->round_trip_delay_samples =
				ranging->round_trip_delay_samples;
		}
		break;
	case SIPPYMODEM_V34_PHASE2_CALLER_WAIT_SECOND_REMOTE_REVERSAL:
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
	struct sippymodem_v34_phase2_ranging *ranging,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	if (ranging->phase !=
	    SIPPYMODEM_V34_PHASE2_CALLER_WAIT_PROBE_START)
		return;
	/* This call already consumed one sample after the estimated reversal
	 * edge. Emit on the final expected tail sample so the next sample can be
	 * passed directly to the probe receiver. */
	if (--ranging->probe_start_delay_samples == 0)
		enter_probe_ready(ranging, event);
}

static void finish_tone_tail(
	struct sippymodem_v34_phase2_ranging *ranging,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	if (ranging->phase != SIPPYMODEM_V34_PHASE2_RESPONSE_TONE_TAIL)
		return;
	if (++ranging->post_reversal_samples <
	    SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES)
		return;
	if (ranging->info0_exchange.local_role ==
	    SIPPYMODEM_V34_CALLING_STATION) {
		ranging->phase =
			SIPPYMODEM_V34_PHASE2_CALLER_WAIT_SECOND_REMOTE_REVERSAL;
	} else {
		ranging->phase = SIPPYMODEM_V34_PHASE2_PROBE_READY;
		event->kind = SIPPYMODEM_V34_PHASE2_PROBE_TRANSMIT_READY;
		event->round_trip_delay_samples =
			ranging->round_trip_delay_samples;
	}
}

static bool measurement_active(
	const struct sippymodem_v34_phase2_ranging *ranging)
{
	return (ranging->phase ==
			SIPPYMODEM_V34_PHASE2_RESPONSE_TONE_TAIL &&
		ranging->info0_exchange.local_role ==
			SIPPYMODEM_V34_CALLING_STATION) ||
	       ranging->phase ==
		       SIPPYMODEM_V34_PHASE2_CALLER_WAIT_SECOND_REMOTE_REVERSAL ||
	       (ranging->phase ==
			SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		ranging->info0_exchange.local_role ==
			SIPPYMODEM_V34_ANSWERING_STATION);
}

static void advance_measurement(
	struct sippymodem_v34_phase2_ranging *ranging,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	if (!measurement_active(ranging))
		return;
	if (ranging->measurement_elapsed_samples <
	    SIPPYMODEM_V34_PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES) {
		ranging->measurement_elapsed_samples++;
		return;
	}
	ranging->phase = SIPPYMODEM_V34_PHASE2_RANGING_TIMED_OUT;
	ranging->response_delay_samples = 0;
	ranging->post_reversal_samples = 0;
	ranging->measurement_elapsed_samples = 0;
	ranging->round_trip_delay_samples = 0;
	ranging->round_trip_delay_valid = false;
	event->kind = SIPPYMODEM_V34_PHASE2_RANGING_TIMEOUT;
}

enum sippymodem_v34_phase2_ranging_result
sippymodem_v34_phase2_ranging_sample(
	struct sippymodem_v34_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_ranging_event *event)
{
	struct sippymodem_v34_phase2_ranging next;
	struct sippymodem_v34_phase2_ranging_event emitted = { 0 };
	enum sippymodem_v34_phase2_info0_transmit_phase transmit_phase_before;
	bool pure_tone_sample;
	int16_t generated;

	if (!sippymodem_v34_phase2_ranging_is_valid(ranging) ||
	    transmit_sample == NULL || event == NULL)
		return SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (!prepare_transmitter(&next))
		return SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT;
	transmit_phase_before = next.info0_exchange.transmit_phase;
	pure_tone_sample = transmit_phase_before ==
			   SIPPYMODEM_V34_PHASE2_INFO0_TONE;
	if (sippymodem_v34_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &emitted.exchange_event) !=
	    SIPPYMODEM_V34_PHASE2_INFO0_EXCHANGE_OK)
		return SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT;
	if (next.phase != SIPPYMODEM_V34_PHASE2_RANGING_INFO0_EXCHANGE &&
	    !phase_transmits_tone(next.phase))
		generated = 0;
	if (next.info0_exchange.local_role ==
		    SIPPYMODEM_V34_ANSWERING_STATION &&
	    next.local_tone_samples <
		    SIPPYMODEM_V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	    pure_tone_sample)
		next.local_tone_samples++;
	enter_active_ranging(&next);
	if (emitted.exchange_event.tone_event.kind ==
	    SIPPYMODEM_V34_PHASE2_TONE_PHASE_REVERSED)
		handle_reversal(
			&next,
			emitted.exchange_event.tone_event.reversal_delay_samples,
			&emitted);
	finish_tone_tail(&next, &emitted);
	advance_measurement(&next, &emitted);
	advance_probe_start(&next, &emitted);
	if (!sippymodem_v34_phase2_ranging_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_RANGING_OK;
}

const char *sippymodem_v34_phase2_ranging_result_name(
	enum sippymodem_v34_phase2_ranging_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_RANGING_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
