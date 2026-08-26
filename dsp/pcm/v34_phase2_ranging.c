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

	if (!v34_phase2_probe_synchronizer_is_valid(
		    &probe->synchronizer) ||
	    (unsigned int)probe->phase >
		    V34_PHASE2_RETRAIN_PROBE_REJECTED)
		return false;
	switch (probe->phase) {
	case V34_PHASE2_RETRAIN_PROBE_DISABLED:
		return probe->boundary_delay_samples == 0 &&
		       synchronizer_is_empty(&probe->synchronizer);
	case V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL:
		return caller_retrain &&
		       probe->boundary_delay_samples == 0 &&
		       synchronizer_is_empty(&probe->synchronizer);
	case V34_PHASE2_RETRAIN_PROBE_WAIT_BOUNDARY:
		return caller_retrain &&
		       probe->boundary_delay_samples > 0 &&
		       probe->boundary_delay_samples <=
			       PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       synchronizer_is_empty(&probe->synchronizer);
	case V34_PHASE2_RETRAIN_PROBE_COLLECTING:
		return caller_retrain &&
		       probe->boundary_delay_samples == 0 &&
		       probe->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING;
	case V34_PHASE2_RETRAIN_PROBE_ACQUIRED:
		return caller_retrain &&
		       probe->boundary_delay_samples == 0 &&
		       probe->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       ranging->control.phase ==
			       PHASE2_RANGING_PROBE_READY &&
		       ranging->control.round_trip_delay_valid;
	case V34_PHASE2_RETRAIN_PROBE_REJECTED:
		return caller_retrain &&
		       probe->boundary_delay_samples == 0 &&
		       probe->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND;
	}
	return false;
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
	       retrain_probe_is_valid(ranging);
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
		.round_trip_delay_valid = true,
	};
	*event = (struct phase2_ranging_event){
		.kind = PHASE2_PROBE_RECEIVE_READY,
	};
	*generated = 0;
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

	if (event->kind != PHASE2_RANGING_NO_EVENT) {
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
	if (phase2_ranging_prepare_sample(
		    &next.control, &next.info0_exchange.transport,
		    next.info0_exchange.peer_info0_received) !=
	    PHASE2_RANGING_OK)
		return V34_PHASE2_RANGING_INVALID_ARGUMENT;
	ranging_phase_before = next.control.phase;
	transmit_phase_before =
		next.info0_exchange.transport.transmit_phase;
	if (v34_phase2_info0_exchange_sample(
		    &next.info0_exchange, received_sample, &generated,
		    &exchange_event) != V34_PHASE2_INFO0_EXCHANGE_OK ||
	    (exchange_event.recovery_started &&
	     phase2_ranging_control_init(
		     &next.control,
		     &next.info0_exchange.transport) !=
		     PHASE2_RANGING_OK) ||
	    (exchange_event.recovery_started && next.retraining &&
	     next.info0_exchange.local_role == V34_CALLING_STATION &&
	     !reset_retrain_probe(
		     &next,
		     V34_PHASE2_RETRAIN_PROBE_WAIT_REVERSAL)) ||
	    phase2_ranging_finish_sample(
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
