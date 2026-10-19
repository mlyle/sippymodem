// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_ranging.h"

#include <stddef.h>

static bool synchronizer_is_empty(
	const struct v34_phase2_probe_synchronizer *synchronizer)
{
	return synchronizer->phase ==
		       V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING &&
	       synchronizer->samples_observed == 0;
}

static bool retrain_probe_is_valid(
	const struct v34_phase2_ranging *ranging)
{
	const struct v34_phase2_retrain_probe *probe =
		&ranging->retrain_probe;
	bool caller_retrain = ranging->retraining &&
		ranging->info0_exchange.local_role == V34_CALLING_STATION;
	bool answerer_trial = !ranging->retraining &&
		ranging->info0_exchange.local_role == V34_ANSWERING_STATION &&
		ranging->control.round_trip_delay_valid &&
		ranging->control.round_trip_delay_samples >=
			V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		!probe->early_boundary_retry_used;

	if (!v34_phase2_probe_synchronizer_is_valid(
		    &probe->synchronizer) ||
	    (unsigned int)probe->phase >
		    V34_PHASE2_RETRAIN_PROBE_REJECTED)
		return false;
	switch (probe->phase) {
	case V34_PHASE2_RETRAIN_PROBE_DISABLED:
		return !probe->early_boundary_retry_used &&
		       probe->boundary_delay_samples == 0 &&
		       synchronizer_is_empty(&probe->synchronizer);
	case V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL:
		return caller_retrain &&
		       !probe->early_boundary_retry_used &&
		       probe->boundary_delay_samples == 0 &&
		       synchronizer_is_empty(&probe->synchronizer);
	case V34_PHASE2_RETRAIN_PROBE_WAIT_BOUNDARY:
		return (caller_retrain || answerer_trial) &&
		       !probe->early_boundary_retry_used &&
		       probe->boundary_delay_samples > 0 &&
		       probe->boundary_delay_samples <=
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       synchronizer_is_empty(&probe->synchronizer);
	case V34_PHASE2_RETRAIN_PROBE_COLLECTING:
		return (caller_retrain || answerer_trial) &&
		       probe->boundary_delay_samples == 0 &&
		       probe->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING;
	case V34_PHASE2_RETRAIN_PROBE_ACQUIRED:
		return (caller_retrain || answerer_trial) &&
		       probe->boundary_delay_samples == 0 &&
		       probe->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       ranging->control.phase ==
			       PHASE2_RANGING_PROBE_READY &&
		       ranging->control.round_trip_delay_valid;
	case V34_PHASE2_RETRAIN_PROBE_REJECTED:
		return (caller_retrain || answerer_trial) &&
		       probe->boundary_delay_samples == 0 &&
		       probe->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND;
	}
	return false;
}

static bool control_is_clear(const struct phase2_ranging_control *control)
{
	return control->phase == PHASE2_RANGING_INFO0_EXCHANGE &&
	       control->local_tone_samples == 0 &&
	       control->deferred_reversal_samples == 0 &&
	       control->response_delay_samples == 0 &&
	       control->post_reversal_samples == 0 &&
	       control->probe_start_delay_samples == 0 &&
	       control->probe_reversal_elapsed_samples == 0 &&
	       control->measurement_elapsed_samples == 0 &&
	       control->round_trip_delay_samples == 0 &&
	       !control->round_trip_delay_valid;
}

static bool control_has_ranging_epoch(
	const struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange)
{
	if (exchange->signal_role == PHASE2_SIGNAL_A) {
		/* A repeated INFO0 invalidates an unanswered local reversal. */
		return control->phase == PHASE2_RESPONSE_DELAY ||
		       control->phase == PHASE2_RESPONSE_TONE_TAIL ||
		       control->phase == PHASE2_RANGING_PROBE_READY;
	}
	return control->phase == PHASE2_RESPONSE_DELAY ||
	       control->phase == PHASE2_RESPONSE_TONE_TAIL ||
	       control->phase == PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL ||
	       control->phase == PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE ||
	       control->phase == PHASE2_SIGNAL_B_WAIT_PROBE_START ||
	       control->phase == PHASE2_RANGING_PROBE_READY;
}

static bool reset_retrain_probe(
	struct v34_phase2_ranging *ranging,
	enum v34_phase2_retrain_probe_phase phase)
{
	if ((phase != V34_PHASE2_RETRAIN_PROBE_DISABLED &&
	     phase != V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL) ||
	    v34_phase2_probe_synchronizer_init(
		    &ranging->retrain_probe.synchronizer) !=
	    V34_PHASE2_PROBE_SYNCHRONIZER_OK)
		return false;
	ranging->retrain_probe.boundary_delay_samples = 0;
	ranging->retrain_probe.phase = phase;
	ranging->retrain_probe.early_boundary_retry_used = false;
	return true;
}

bool v34_phase2_ranging_is_valid(
	const struct v34_phase2_ranging *ranging)
{
	return ranging != NULL &&
	       v34_phase2_info0_exchange_is_valid(
		       &ranging->info0_exchange) &&
	       (!ranging->retraining ||
		ranging->info0_exchange.peer_info0_received) &&
	       phase2_ranging_control_is_valid(
		       &ranging->control,
		       &ranging->info0_exchange.transport,
		       ranging->info0_exchange.peer_info0_received) &&
	       (ranging->recovery_paused ?
		       (ranging->info0_exchange.recovery_active ||
			ranging->info0_exchange.acknowledgements_complete) &&
		       control_has_ranging_epoch(
			       &ranging->recovery_control,
			       &ranging->info0_exchange.transport) &&
		       phase2_ranging_control_is_valid(
			       &ranging->recovery_control,
			       &ranging->info0_exchange.transport,
			       ranging->info0_exchange.peer_info0_received) :
		       control_is_clear(&ranging->recovery_control)) &&
	       retrain_probe_is_valid(ranging);
}

static bool pause_ranging_for_recovery(struct v34_phase2_ranging *ranging)
{
	struct phase2_ranging_control retained = ranging->control;
	bool pause = control_has_ranging_epoch(
		&retained, &ranging->info0_exchange.transport);

	if (ranging->info0_exchange.local_role == V34_ANSWERING_STATION &&
	    !reset_retrain_probe(ranging, V34_PHASE2_RETRAIN_PROBE_DISABLED))
		return false;
	if (phase2_ranging_control_init(
		    &ranging->control,
		    &ranging->info0_exchange.transport) != PHASE2_RANGING_OK)
		return false;
	if (pause) {
		ranging->recovery_control = retained;
		ranging->recovery_paused = true;
	}
	return true;
}

static bool resume_ranging_after_recovery(struct v34_phase2_ranging *ranging)
{
	if (!ranging->recovery_paused)
		return true;
	ranging->control = ranging->recovery_control;
	ranging->recovery_control = (struct phase2_ranging_control){ 0 };
	ranging->recovery_paused = false;
	return phase2_ranging_control_is_valid(
		&ranging->control, &ranging->info0_exchange.transport,
		ranging->info0_exchange.peer_info0_received);
}

static bool recovery_tone_handoff_ready(
	const struct v34_phase2_ranging *ranging,
	const struct v34_phase2_info0_exchange_event *event)
{
	return ranging->recovery_paused &&
	       !ranging->info0_exchange.recovery_active &&
	       (event->tone_event.kind == V34_PHASE2_TONE_STARTED ||
		event->tone_event.kind == V34_PHASE2_TONE_PHASE_REVERSED);
}

enum v34_phase2_ranging_result
v34_phase2_ranging_init(
	struct v34_phase2_ranging *ranging,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0)
{
	struct v34_phase2_ranging initialized = { 0 };

	if (ranging == NULL ||
	    v34_phase2_info0_exchange_init(
		    &initialized.info0_exchange, local_role,
		    nominal_amplitude, local_info0) !=
		    V34_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized.control,
		    &initialized.info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !reset_retrain_probe(
		    &initialized,
		    V34_PHASE2_RETRAIN_PROBE_DISABLED) ||
	    !v34_phase2_ranging_is_valid(&initialized))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = initialized;
	return V34_PHASE2_RANGING_OK;
}

enum v34_phase2_ranging_result
v34_phase2_ranging_init_retrain(
	struct v34_phase2_ranging *ranging,
	enum v34_station_role local_role, int16_t nominal_amplitude,
	const struct v34_info0 *local_info0,
	const struct v34_info0 *peer_info0)
{
	struct v34_phase2_ranging initialized = {
		.retraining = true,
	};

	if (ranging == NULL ||
	    v34_phase2_info0_exchange_init_retrain(
		    &initialized.info0_exchange, local_role,
		    nominal_amplitude, local_info0, peer_info0) !=
		    V34_PHASE2_INFO0_EXCHANGE_OK ||
	    phase2_ranging_control_init(
		    &initialized.control,
		    &initialized.info0_exchange.transport) !=
		    PHASE2_RANGING_OK ||
	    !reset_retrain_probe(
		    &initialized,
		    local_role == V34_CALLING_STATION ?
			    V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL :
			    V34_PHASE2_RETRAIN_PROBE_DISABLED) ||
	    !v34_phase2_ranging_is_valid(&initialized))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	*ranging = initialized;
	return V34_PHASE2_RANGING_OK;
}

static bool arm_retrain_probe_trial(
	struct v34_phase2_ranging *ranging,
	const struct v34_phase2_tone_event *tone_event)
{
	struct v34_phase2_retrain_probe *probe =
		&ranging->retrain_probe;
	uint16_t delay = tone_event->reversal_delay_samples;

	if (delay > PHASE2_RANGING_TONE_TAIL_SAMPLES)
		return reset_retrain_probe(
			ranging, V34_PHASE2_RETRAIN_PROBE_DISABLED);
	probe->boundary_delay_samples =
		PHASE2_RANGING_TONE_TAIL_SAMPLES - delay;
	probe->phase = probe->boundary_delay_samples == 0 ?
		V34_PHASE2_RETRAIN_PROBE_COLLECTING :
		V34_PHASE2_RETRAIN_PROBE_WAIT_BOUNDARY;
	return true;
}

static void accept_retrain_probe(
	struct v34_phase2_ranging *ranging,
	struct phase2_ranging_event *event, int16_t *generated)
{
	ranging->retrain_probe.phase =
		V34_PHASE2_RETRAIN_PROBE_ACQUIRED;
	ranging->control = (struct phase2_ranging_control){
		.phase = PHASE2_RANGING_PROBE_READY,
		.post_reversal_samples =
			PHASE2_RANGING_TONE_TAIL_SAMPLES,
		/* The captured L1 edge dates the preceding 10 ms reversal tail. */
		.probe_reversal_elapsed_samples = PHASE2_RANGING_TONE_TAIL_SAMPLES +
			V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES -
			ranging->retrain_probe.synchronizer.leading_samples,
		.round_trip_delay_valid = true,
	};
	*event = (struct phase2_ranging_event){
		.kind = PHASE2_PROBE_RECEIVE_READY,
	};
	*generated = 0;
}

static bool restart_retrain_probe_trial(
	struct v34_phase2_retrain_probe *probe)
{
	if (probe->early_boundary_retry_used ||
	    v34_phase2_probe_synchronizer_init(&probe->synchronizer) !=
		    V34_PHASE2_PROBE_SYNCHRONIZER_OK)
		return false;
	probe->boundary_delay_samples = 0;
	probe->phase = V34_PHASE2_RETRAIN_PROBE_COLLECTING;
	probe->early_boundary_retry_used = true;
	return true;
}

static bool sample_retrain_probe_trial(
	struct v34_phase2_ranging *ranging,
	enum phase2_ranging_phase phase_before,
	const struct v34_phase2_tone_event *tone_event,
	int16_t received_sample, int16_t *generated,
	struct phase2_ranging_event *event)
{
	struct v34_phase2_retrain_probe *probe =
		&ranging->retrain_probe;
	struct v34_phase2_probe_synchronizer_event probe_event;
	bool answerer = ranging->info0_exchange.local_role == V34_ANSWERING_STATION;

	/* A recovering caller can begin L1 after this first B reversal, before
	 * our own probe starts. Retain one bounded prefix, but never replace the
	 * answerer's transmit schedule with the caller's retrain shortcut. */
	if (answerer && !ranging->retraining &&
	    event->kind == PHASE2_ROUND_TRIP_DELAY_MEASURED &&
	    event->round_trip_delay_samples >=
		    V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES)
		return reset_retrain_probe(ranging, V34_PHASE2_RETRAIN_PROBE_DISABLED) &&
		       arm_retrain_probe_trial(ranging, tone_event);
	if (event->kind != PHASE2_RANGING_NO_EVENT &&
	    !(answerer && event->kind == PHASE2_PROBE_TRANSMIT_READY)) {
		if (probe->phase != V34_PHASE2_RETRAIN_PROBE_ACQUIRED &&
		    !reset_retrain_probe(
			    ranging,
			    V34_PHASE2_RETRAIN_PROBE_DISABLED))
			return false;
		return true;
	}
	if ((probe->phase ==
		     V34_PHASE2_RETRAIN_PROBE_WAIT_BOUNDARY ||
	     probe->phase == V34_PHASE2_RETRAIN_PROBE_COLLECTING) &&
	    phase_before == PHASE2_RESPONSE_DELAY &&
	    ranging->control.phase ==
		    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
	    tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED)
		return reset_retrain_probe(
			ranging,
			V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL);
	if (probe->phase ==
		    V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL &&
	    phase_before == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
	    ranging->control.phase == PHASE2_RESPONSE_DELAY &&
	    tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED &&
	    !arm_retrain_probe_trial(ranging, tone_event))
		return false;
	/* Some peers retain Tone A after the first ranging reversal and begin L1
	 * on a later edge that is too early to be a new round-trip measurement.
	 * The timing controller correctly rejects that edge; use it once as a
	 * stronger probe boundary and let the spectrum check decide. */
	if (!probe->early_boundary_retry_used &&
	    (probe->phase == V34_PHASE2_RETRAIN_PROBE_WAIT_BOUNDARY ||
	     probe->phase == V34_PHASE2_RETRAIN_PROBE_COLLECTING ||
	     probe->phase == V34_PHASE2_RETRAIN_PROBE_REJECTED) &&
	    phase_before == PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL &&
	    ranging->control.phase ==
		    PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL &&
	    tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED &&
	    !restart_retrain_probe_trial(probe))
		return false;
	if (probe->phase ==
	    V34_PHASE2_RETRAIN_PROBE_WAIT_BOUNDARY) {
		probe->boundary_delay_samples--;
		if (probe->boundary_delay_samples != 0)
			return true;
		probe->phase =
			V34_PHASE2_RETRAIN_PROBE_COLLECTING;
		return true;
	}
	if (probe->phase != V34_PHASE2_RETRAIN_PROBE_COLLECTING)
		return true;
	if (v34_phase2_probe_synchronizer_sample(
		    &probe->synchronizer, received_sample,
		    &probe_event) !=
	    V34_PHASE2_PROBE_SYNCHRONIZER_OK)
		return false;
	switch (probe_event.kind) {
	case V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE:
		break;
	case V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED:
		if (answerer)
			probe->phase = V34_PHASE2_RETRAIN_PROBE_ACQUIRED;
		else
			accept_retrain_probe(ranging, event, generated);
		break;
	case V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND:
		probe->phase =
			V34_PHASE2_RETRAIN_PROBE_REJECTED;
		break;
	}
	return true;
}

enum v34_phase2_ranging_result
v34_phase2_ranging_sample(
	struct v34_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_ranging_event *event)
{
	struct v34_phase2_ranging next;
	struct v34_phase2_info0_exchange_event exchange_event;
	struct phase2_ranging_event ranging_event;
	struct v34_phase2_ranging_event emitted;
	enum phase2_info0_transmit_phase transmit_phase_before;
	enum phase2_ranging_phase ranging_phase_before;
	int16_t generated;

	if (!v34_phase2_ranging_is_valid(ranging) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	next = *ranging;
	if (phase2_ranging_prepare_sample_owned(
		    &next.control, &next.info0_exchange.transport) !=
	    PHASE2_RANGING_OK)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	ranging_phase_before = next.control.phase;
	transmit_phase_before =
		next.info0_exchange.transport.transmit_phase;
	if (v34_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != V34_PHASE2_INFO0_EXCHANGE_OK ||
	    (exchange_event.recovery_started &&
	     !pause_ranging_for_recovery(&next)) ||
	    (recovery_tone_handoff_ready(&next, &exchange_event) &&
	     !resume_ranging_after_recovery(&next)) ||
	    (exchange_event.recovery_started && next.retraining &&
	     next.info0_exchange.local_role == V34_CALLING_STATION &&
	     !reset_retrain_probe(
		     &next,
		     V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL)) ||
	    phase2_ranging_finish_sample_owned(
		    &next.control, &next.info0_exchange.transport,
		    next.info0_exchange.peer_info0_received,
		    transmit_phase_before, &exchange_event.tone_event,
		    &generated, &ranging_event) != PHASE2_RANGING_OK ||
	    !sample_retrain_probe_trial(
		    &next, ranging_phase_before,
		    &exchange_event.tone_event, received_sample,
		    &generated, &ranging_event) ||
	    !v34_phase2_ranging_is_valid(&next))
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	emitted = (struct v34_phase2_ranging_event){
		.exchange_event = exchange_event,
		.kind = ranging_event.kind,
		.round_trip_delay_samples =
			ranging_event.round_trip_delay_samples,
	};
	*ranging = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_RANGING_OK;
}
