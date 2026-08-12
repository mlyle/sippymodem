// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2.h"

#include <stddef.h>

static bool failure_is_valid(enum v34_phase2_failure failure)
{
	return failure >= V34_PHASE2_FAILURE_NONE &&
	       failure <= V34_PHASE2_FAILURE_INTERNAL;
}

static enum v34_station_role local_role(const struct v34_phase2 *phase2)
{
	switch (phase2->stage) {
	case V34_PHASE2_RANGING:
		return phase2->state.ranging.info0_exchange.local_role;
	case V34_PHASE2_INITIAL_PROBE:
		return phase2->state.initial_probe.ranging.info0_exchange
			.local_role;
	case V34_PHASE2_PROBE_TURNAROUND:
		return phase2->state.turnaround.initial_probe.ranging
			.info0_exchange.local_role;
	case V34_PHASE2_RECIPROCAL_PROBE:
		return phase2->state.reciprocal_probe.turnaround.initial_probe
			.ranging.info0_exchange.local_role;
	}
	return (enum v34_station_role)-1;
}

static enum v34_phase2_failure initial_probe_failure(
	enum v34_phase2_initial_probe_failure failure)
{
	switch (failure) {
	case V34_PHASE2_INITIAL_PROBE_NO_FAILURE:
		return V34_PHASE2_FAILURE_NONE;
	case V34_PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND:
		return V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND;
	case V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT:
		return V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
	case V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT:
		return V34_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT;
	}
	return V34_PHASE2_FAILURE_INTERNAL;
}

static enum v34_phase2_failure active_failure(
	const struct v34_phase2 *phase2)
{
	switch (phase2->stage) {
	case V34_PHASE2_RANGING:
		return phase2->state.ranging.phase ==
			       V34_PHASE2_RANGING_TIMED_OUT ?
			       V34_PHASE2_FAILURE_RANGING_TIMEOUT :
			       V34_PHASE2_FAILURE_NONE;
	case V34_PHASE2_INITIAL_PROBE:
		return initial_probe_failure(
			phase2->state.initial_probe.failure);
	case V34_PHASE2_PROBE_TURNAROUND:
		return phase2->state.turnaround.failure ==
			       V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT ?
			       V34_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT :
			       V34_PHASE2_FAILURE_NONE;
	case V34_PHASE2_RECIPROCAL_PROBE:
		switch (phase2->state.reciprocal_probe.failure) {
		case V34_PHASE2_RECIPROCAL_PROBE_NO_FAILURE:
			return V34_PHASE2_FAILURE_NONE;
		case V34_PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND:
			return V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND;
		case V34_PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT:
			return V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
		}
		break;
	}
	return V34_PHASE2_FAILURE_INTERNAL;
}

bool v34_phase2_is_valid(const struct v34_phase2 *phase2)
{
	enum v34_phase2_failure child_failure;
	bool child_valid;

	if (phase2 == NULL || !failure_is_valid(phase2->failure))
		return false;
	switch (phase2->stage) {
	case V34_PHASE2_RANGING:
		child_valid = v34_phase2_ranging_is_valid(
			&phase2->state.ranging);
		break;
	case V34_PHASE2_INITIAL_PROBE:
		child_valid = v34_phase2_initial_probe_is_valid(
			&phase2->state.initial_probe);
		break;
	case V34_PHASE2_PROBE_TURNAROUND:
		child_valid = v34_phase2_probe_turnaround_is_valid(
			&phase2->state.turnaround);
		break;
	case V34_PHASE2_RECIPROCAL_PROBE:
		child_valid = v34_phase2_reciprocal_probe_is_valid(
			&phase2->state.reciprocal_probe);
		break;
	default:
		return false;
	}
	if (!child_valid)
		return false;
	child_failure = active_failure(phase2);
	return phase2->failure == V34_PHASE2_FAILURE_INTERNAL ?
		       child_failure == V34_PHASE2_FAILURE_NONE :
		       phase2->failure == child_failure;
}

bool v34_phase2_has_failed(const struct v34_phase2 *phase2)
{
	return v34_phase2_is_valid(phase2) &&
	       phase2->failure != V34_PHASE2_FAILURE_NONE;
}

static bool ready_for_info1(const struct v34_phase2 *phase2)
{
	if (phase2->failure != V34_PHASE2_FAILURE_NONE ||
	    phase2->stage != V34_PHASE2_RECIPROCAL_PROBE)
		return false;
	return local_role(phase2) == V34_CALLING_STATION ?
		       phase2->state.reciprocal_probe.phase ==
			       V34_PHASE2_RECIPROCAL_PROBE_CALLER_COMPLETE :
		       phase2->state.reciprocal_probe.phase ==
			       V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A;
}

bool v34_phase2_is_ready_for_info1(const struct v34_phase2 *phase2)
{
	return v34_phase2_is_valid(phase2) && ready_for_info1(phase2);
}

enum v34_phase2_result v34_phase2_init(
	struct v34_phase2 *phase2, enum v34_station_role role,
	int16_t nominal_amplitude, const struct v34_info0 *local_info0)
{
	struct v34_phase2 initialized = {
		.stage = V34_PHASE2_RANGING,
	};

	if (phase2 == NULL ||
	    v34_phase2_ranging_init(
		    &initialized.state.ranging, role, nominal_amplitude,
		    local_info0) != V34_PHASE2_RANGING_OK ||
	    !v34_phase2_is_valid(&initialized))
		return V34_PHASE2_INVALID_ARGUMENT;
	*phase2 = initialized;
	return V34_PHASE2_OK;
}

static void fail_internal(
	struct v34_phase2 *phase2, struct v34_phase2_event *event)
{
	phase2->failure = V34_PHASE2_FAILURE_INTERNAL;
	event->kind = V34_PHASE2_EVENT_FAILED;
	event->failure = phase2->failure;
}

static bool enter_initial_probe(
	struct v34_phase2 *phase2, struct v34_phase2_event *event)
{
	struct v34_phase2_initial_probe next;

	if (v34_phase2_initial_probe_init(
		    &next, &phase2->state.ranging) !=
	    V34_PHASE2_INITIAL_PROBE_OK)
		return false;
	phase2->state.initial_probe = next;
	phase2->stage = V34_PHASE2_INITIAL_PROBE;
	event->kind = V34_PHASE2_EVENT_INITIAL_PROBE_STARTED;
	return true;
}

static bool enter_turnaround(
	struct v34_phase2 *phase2, struct v34_phase2_event *event)
{
	struct v34_phase2_probe_turnaround next;

	if (v34_phase2_probe_turnaround_init(
		    &next, &phase2->state.initial_probe) !=
	    V34_PHASE2_PROBE_TURNAROUND_OK)
		return false;
	phase2->state.turnaround = next;
	phase2->stage = V34_PHASE2_PROBE_TURNAROUND;
	event->kind = V34_PHASE2_EVENT_PROBE_TURNAROUND_STARTED;
	return true;
}

static bool enter_reciprocal_probe(
	struct v34_phase2 *phase2, struct v34_phase2_event *event)
{
	struct v34_phase2_reciprocal_probe next;

	if (v34_phase2_reciprocal_probe_init(
		    &next, &phase2->state.turnaround) !=
	    V34_PHASE2_RECIPROCAL_PROBE_OK)
		return false;
	phase2->state.reciprocal_probe = next;
	phase2->stage = V34_PHASE2_RECIPROCAL_PROBE;
	event->kind = V34_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED;
	return true;
}

static bool sample_ranging(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_ranging_event child;

	if (v34_phase2_ranging_sample(
		    &phase2->state.ranging, received_sample,
		    transmit_sample, &child) != V34_PHASE2_RANGING_OK)
		return false;
	switch (child.kind) {
	case V34_PHASE2_ROUND_TRIP_DELAY_MEASURED:
		event->kind = V34_PHASE2_EVENT_ROUND_TRIP_DELAY;
		event->round_trip_delay_samples =
			child.round_trip_delay_samples;
		break;
	case V34_PHASE2_PROBE_RECEIVE_READY:
	case V34_PHASE2_PROBE_TRANSMIT_READY:
		return enter_initial_probe(phase2, event);
	case V34_PHASE2_RANGING_TIMEOUT:
		phase2->failure = V34_PHASE2_FAILURE_RANGING_TIMEOUT;
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
		break;
	case V34_PHASE2_RANGING_NO_EVENT:
		break;
	}
	return true;
}

static bool sample_initial_probe(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_initial_probe_event child;
	enum v34_station_role role = local_role(phase2);

	if (v34_phase2_initial_probe_sample(
		    &phase2->state.initial_probe, received_sample,
		    transmit_sample, &child) != V34_PHASE2_INITIAL_PROBE_OK)
		return false;
	if (child.kind == V34_PHASE2_INITIAL_PROBE_EVENT_FAILED) {
		phase2->failure = initial_probe_failure(child.failure);
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
	} else if (child.kind ==
		   V34_PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE) {
		event->kind =
			V34_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED;
	} else if ((role == V34_CALLING_STATION &&
		    child.kind ==
			    V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE) ||
		   (role == V34_ANSWERING_STATION &&
		    child.kind ==
			    V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE)) {
		return enter_turnaround(phase2, event);
	}
	return true;
}

static bool sample_turnaround(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_probe_turnaround_event child;

	if (v34_phase2_probe_turnaround_sample(
		    &phase2->state.turnaround, received_sample,
		    transmit_sample, &child) !=
	    V34_PHASE2_PROBE_TURNAROUND_OK)
		return false;
	if (child.kind == V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED) {
		phase2->failure = V34_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT;
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
	} else if (child.kind ==
			   V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY ||
		   child.kind ==
			   V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY) {
		return enter_reciprocal_probe(phase2, event);
	}
	return true;
}

static bool sample_reciprocal_probe(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_reciprocal_probe_event child;

	if (v34_phase2_reciprocal_probe_sample(
		    &phase2->state.reciprocal_probe, received_sample,
		    transmit_sample, &child) !=
	    V34_PHASE2_RECIPROCAL_PROBE_OK)
		return false;
	if (child.kind == V34_PHASE2_RECIPROCAL_PROBE_EVENT_FAILED) {
		phase2->failure = child.failure ==
					  V34_PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND ?
				  V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND :
				  V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
	} else if (child.kind ==
			   V34_PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED ||
		   child.kind ==
			   V34_PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED) {
		event->kind = V34_PHASE2_EVENT_READY_FOR_INFO1;
	}
	return true;
}

enum v34_phase2_result v34_phase2_sample(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_event emitted = { 0 };
	int16_t generated = 0;
	bool accepted;

	if (!v34_phase2_is_valid(phase2) || transmit_sample == NULL ||
	    event == NULL)
		return V34_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != V34_PHASE2_FAILURE_NONE) {
		*transmit_sample = 0;
		*event = emitted;
		return V34_PHASE2_OK;
	}
	switch (phase2->stage) {
	case V34_PHASE2_RANGING:
		accepted = sample_ranging(
			phase2, received_sample, &generated, &emitted);
		break;
	case V34_PHASE2_INITIAL_PROBE:
		accepted = sample_initial_probe(
			phase2, received_sample, &generated, &emitted);
		break;
	case V34_PHASE2_PROBE_TURNAROUND:
		accepted = sample_turnaround(
			phase2, received_sample, &generated, &emitted);
		break;
	case V34_PHASE2_RECIPROCAL_PROBE:
		accepted = sample_reciprocal_probe(
			phase2, received_sample, &generated, &emitted);
		break;
	default:
		return V34_PHASE2_INVALID_ARGUMENT;
	}
	if (!accepted || !v34_phase2_is_valid(phase2)) {
		fail_internal(phase2, &emitted);
		if (!v34_phase2_is_valid(phase2))
			return V34_PHASE2_INVALID_STATE;
	}
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_measure_remote_probe(
	const struct v34_phase2 *phase2,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v34_phase2_is_valid(phase2) || measurement == NULL)
		return V34_PHASE2_INVALID_ARGUMENT;
	if (!ready_for_info1(phase2))
		return V34_PHASE2_INVALID_STATE;
	if (local_role(phase2) == V34_CALLING_STATION) {
		const struct v34_phase2_initial_probe *probe =
			&phase2->state.reciprocal_probe.turnaround.initial_probe;

		return v34_phase2_initial_probe_measure(probe, measurement) ==
			       V34_PHASE2_INITIAL_PROBE_OK ?
			       V34_PHASE2_OK :
			       V34_PHASE2_INVALID_STATE;
	}
	return v34_phase2_reciprocal_probe_measure(
		       &phase2->state.reciprocal_probe, measurement) ==
		       V34_PHASE2_RECIPROCAL_PROBE_OK ?
		       V34_PHASE2_OK :
		       V34_PHASE2_INVALID_STATE;
}

const char *v34_phase2_result_name(enum v34_phase2_result result)
{
	switch (result) {
	case V34_PHASE2_OK:
		return "ok";
	case V34_PHASE2_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE2_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}

const char *v34_phase2_failure_name(enum v34_phase2_failure failure)
{
	switch (failure) {
	case V34_PHASE2_FAILURE_NONE:
		return "none";
	case V34_PHASE2_FAILURE_RANGING_TIMEOUT:
		return "ranging timeout";
	case V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND:
		return "remote probe not found";
	case V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT:
		return "local probe limit";
	case V34_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT:
		return "remote tone timeout";
	case V34_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT:
		return "remote reversal timeout";
	case V34_PHASE2_FAILURE_INTERNAL:
		return "internal failure";
	}
	return "unknown";
}
