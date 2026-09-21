// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2.h"

#include <stddef.h>

static bool failure_is_valid(enum v34_phase2_failure failure)
{
	return failure >= V34_PHASE2_FAILURE_NONE &&
	       failure <= V34_PHASE2_FAILURE_INTERNAL;
}

static const struct v34_info0 *caller_info0(
	const struct v34_phase2 *phase2)
{
	return phase2->local_role == V34_CALLING_STATION ?
		       &phase2->local_info0 : &phase2->peer_info0;
}

static const struct v34_info0 *answerer_info0(
	const struct v34_phase2 *phase2)
{
	return phase2->local_role == V34_ANSWERING_STATION ?
		       &phase2->local_info0 : &phase2->peer_info0;
}

static enum v34_phase2_failure initial_probe_failure(
	enum phase2_initial_probe_failure failure)
{
	switch (failure) {
	case PHASE2_INITIAL_PROBE_NO_FAILURE:
		return V34_PHASE2_FAILURE_NONE;
	case PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND:
		return V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND;
	case PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT:
		return V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
	}
	return V34_PHASE2_FAILURE_INTERNAL;
}

static bool info1_records_are_consistent(
	const struct v34_phase2 *phase2)
{
	const struct v34_phase2_info1_exchange *exchange =
		&phase2->state.info1;
	const struct v34_info1c *info1c;

	if (!exchange->peer_parameters_received)
		return true;
	info1c = phase2->local_role == V34_CALLING_STATION ?
			 &phase2->local_info1.info1c :
			 &exchange->peer_parameters.info1c;
	if (!v34_phase2_info1c_is_consistent(
		    caller_info0(phase2), answerer_info0(phase2), info1c))
		return false;
	if (phase2->local_role == V34_CALLING_STATION)
		return v34_phase2_info1a_is_consistent(
			caller_info0(phase2), answerer_info0(phase2),
			info1c, &exchange->peer_parameters.info1a);
	return !phase2->local_info1_stored ||
	       v34_phase2_info1a_is_consistent(
		       caller_info0(phase2), answerer_info0(phase2), info1c,
		       &phase2->local_info1.info1a);
}

static enum v34_phase2_failure active_failure(
	const struct v34_phase2 *phase2)
{
	switch (phase2->stage) {
	case V34_PHASE2_RANGING:
		return V34_PHASE2_FAILURE_NONE;
	case V34_PHASE2_INITIAL_PROBE:
		return initial_probe_failure(
			phase2->state.initial_probe.control.failure);
	case V34_PHASE2_PROBE_TURNAROUND:
		return phase2->state.turnaround.control.failure ==
			       PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT ?
			       V34_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT :
			       V34_PHASE2_FAILURE_NONE;
	case V34_PHASE2_RECIPROCAL_PROBE:
		switch (phase2->state.reciprocal_probe.control.failure) {
		case PHASE2_RECIPROCAL_PROBE_NO_FAILURE:
			return V34_PHASE2_FAILURE_NONE;
		case PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND:
			return V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND;
		case PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT:
			return V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
		}
		break;
	case V34_PHASE2_INFO1:
		if (phase2->state.info1.control.failure ==
		    PHASE2_INFO1_PEER_TIMEOUT)
			return V34_PHASE2_FAILURE_INFO1_TIMEOUT;
		if (!info1_records_are_consistent(phase2))
			return V34_PHASE2_FAILURE_INCONSISTENT_INFO1;
		return V34_PHASE2_FAILURE_NONE;
	}
	return V34_PHASE2_FAILURE_INTERNAL;
}

static enum v34_station_role child_role(
	const struct v34_phase2 *phase2)
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
	case V34_PHASE2_INFO1:
		return v34_phase2_station_for_signal(
			phase2->state.info1.control.signal_role);
	}
	return (enum v34_station_role)-1;
}

static bool local_info1_is_valid(const struct v34_phase2 *phase2)
{
	bool expected;

	if (phase2->stage != V34_PHASE2_INFO1)
		return !phase2->local_info1_stored;
	expected = phase2->local_role == V34_CALLING_STATION ||
		phase2_info1_has_second_record(&phase2->state.info1.control);
	if (phase2->local_info1_stored != expected)
		return false;
	if (!expected)
		return true;
	return phase2->local_role == V34_CALLING_STATION ?
		       v34_info1c_is_valid(&phase2->local_info1.info1c) :
		       v34_info1a_is_valid(&phase2->local_info1.info1a);
}

static bool concurrent_probe_receive_matches_stage(
	const struct v34_phase2 *phase2)
{
	bool child_enabled = phase2->concurrent_probe_receive;
	uint16_t round_trip_delay = 0;

	if (child_enabled &&
	    phase2->local_role != V34_ANSWERING_STATION)
		return false;
	switch (phase2->stage) {
	case V34_PHASE2_RANGING:
		return !child_enabled ||
		       phase2->prior_round_trip_delay_samples != 0;
	case V34_PHASE2_INITIAL_PROBE:
		round_trip_delay = phase2->state.initial_probe.ranging.control
					   .round_trip_delay_samples;
		if (phase2->state.initial_probe.concurrent_receive_enabled !=
		    child_enabled)
			return false;
		break;
	case V34_PHASE2_PROBE_TURNAROUND:
		round_trip_delay = phase2->state.turnaround.initial_probe.ranging
					   .control.round_trip_delay_samples;
		if (phase2->state.turnaround.initial_probe
			    .concurrent_receive_enabled != child_enabled)
			return false;
		break;
	case V34_PHASE2_RECIPROCAL_PROBE:
		round_trip_delay = phase2->state.reciprocal_probe.turnaround
					   .initial_probe.ranging.control
					   .round_trip_delay_samples;
		if (phase2->state.reciprocal_probe.turnaround.initial_probe
			    .concurrent_receive_enabled != child_enabled)
			return false;
		break;
	case V34_PHASE2_INFO1:
		return !child_enabled;
	}
	return !child_enabled || phase2->prior_round_trip_delay_samples != 0 ||
	       round_trip_delay >=
		       V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
}

bool v34_phase2_is_valid(const struct v34_phase2 *phase2)
{
	enum v34_phase2_failure child_failure;
	bool child_valid;

	if (phase2 == NULL || !failure_is_valid(phase2->failure) ||
	    !v34_station_role_is_valid(phase2->local_role) ||
	    phase2->prior_round_trip_delay_samples >
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			    PHASE2_RANGING_TURNAROUND_SAMPLES ||
	    !v34_info0_is_valid(&phase2->local_info0) ||
	    (phase2->peer_info0_stored &&
	     !v34_info0_is_valid(&phase2->peer_info0)) ||
	    (phase2->remote_probe_stored &&
	     (!v34_phase2_probe_measurement_is_valid(
		      &phase2->remote_probe) ||
	      phase2->remote_receive_gain <= 0)) ||
	    (!phase2->remote_probe_stored &&
	     phase2->remote_receive_gain != 0) ||
	    !local_info1_is_valid(phase2) ||
	    !concurrent_probe_receive_matches_stage(phase2) ||
	    (phase2->stage != V34_PHASE2_RANGING &&
	     !phase2->peer_info0_stored))
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
	case V34_PHASE2_INFO1:
		child_valid = phase2->peer_info0_stored &&
			      phase2->remote_probe_stored &&
			      v34_phase2_info1_exchange_is_valid(
				      &phase2->state.info1);
		break;
	default:
		return false;
	}
	if (!child_valid || child_role(phase2) != phase2->local_role)
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
	return phase2->local_role == V34_CALLING_STATION ?
		       phase2->state.reciprocal_probe.control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE :
		       phase2->state.reciprocal_probe.control.phase ==
			       PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE;
}

bool v34_phase2_is_ready_for_info1(const struct v34_phase2 *phase2)
{
	return v34_phase2_is_valid(phase2) && ready_for_info1(phase2);
}

static bool ready_for_phase3(const struct v34_phase2 *phase2)
{
	return phase2->failure == V34_PHASE2_FAILURE_NONE &&
	       phase2->stage == V34_PHASE2_INFO1 &&
	       phase2->state.info1.control.phase == PHASE2_INFO1_COMPLETE &&
	       phase2->local_info1_stored &&
	       info1_records_are_consistent(phase2);
}

bool v34_phase2_is_ready_for_phase3(const struct v34_phase2 *phase2)
{
	return v34_phase2_is_valid(phase2) && ready_for_phase3(phase2);
}

enum v34_phase2_result v34_phase2_init(
	struct v34_phase2 *phase2, enum v34_station_role role,
	int16_t nominal_amplitude, const struct v34_info0 *local_info0)
{
	struct v34_phase2 initialized = {
		.local_role = role,
		.stage = V34_PHASE2_RANGING,
	};

	if (phase2 == NULL || !v34_info0_is_valid(local_info0))
		return V34_PHASE2_INVALID_ARGUMENT;
	initialized.local_info0 = *local_info0;
	initialized.local_info0.acknowledges_peer_info0 = false;
	if (v34_phase2_ranging_init(
		    &initialized.state.ranging, role, nominal_amplitude,
		    &initialized.local_info0) != V34_PHASE2_RANGING_OK ||
	    !v34_phase2_is_valid(&initialized))
		return V34_PHASE2_INVALID_ARGUMENT;
	*phase2 = initialized;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_init_retrain(
	struct v34_phase2 *phase2, enum v34_station_role role,
	int16_t nominal_amplitude, const struct v34_info0 *local_info0,
	const struct v34_info0 *peer_info0,
	uint16_t prior_round_trip_delay_samples)
{
	struct v34_phase2 initialized = {
		.prior_round_trip_delay_samples =
			prior_round_trip_delay_samples,
		.local_role = role,
		.stage = V34_PHASE2_RANGING,
		.peer_info0_stored = true,
	};

	if (phase2 == NULL || !v34_info0_is_valid(local_info0) ||
	    !v34_info0_is_valid(peer_info0) ||
	    prior_round_trip_delay_samples >
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
			    PHASE2_RANGING_TURNAROUND_SAMPLES)
		return V34_PHASE2_INVALID_ARGUMENT;
	initialized.local_info0 = *local_info0;
	initialized.local_info0.acknowledges_peer_info0 = true;
	initialized.peer_info0 = *peer_info0;
	if (v34_phase2_ranging_init_retrain(
		    &initialized.state.ranging, role, nominal_amplitude,
		    &initialized.local_info0, &initialized.peer_info0) !=
		    V34_PHASE2_RANGING_OK ||
	    !v34_phase2_is_valid(&initialized))
		return V34_PHASE2_INVALID_ARGUMENT;
	*phase2 = initialized;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_peer_info0(
	const struct v34_phase2 *phase2, struct v34_info0 *peer_info0)
{
	if (!v34_phase2_is_valid(phase2) || peer_info0 == NULL)
		return V34_PHASE2_INVALID_ARGUMENT;
	if (!phase2->peer_info0_stored)
		return V34_PHASE2_INVALID_STATE;
	*peer_info0 = phase2->peer_info0;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_start_caller_info1(
	struct v34_phase2 *phase2, const struct v34_info1c *local_info1c)
{
	struct v34_phase2_info1_exchange exchange;
	struct v34_phase2 next;

	if (!v34_phase2_is_valid(phase2) ||
	    !v34_info1c_is_valid(local_info1c))
		return V34_PHASE2_INVALID_ARGUMENT;
	if (phase2->local_role != V34_CALLING_STATION ||
	    !ready_for_info1(phase2) || !phase2->peer_info0_stored ||
	    !phase2->remote_probe_stored)
		return V34_PHASE2_INVALID_STATE;
	if (!v34_phase2_info1c_is_consistent(
		    caller_info0(phase2), answerer_info0(phase2), local_info1c))
		return V34_PHASE2_INVALID_ARGUMENT;
	if (v34_phase2_info1_exchange_caller_init(
		    &exchange, &phase2->state.reciprocal_probe,
		    local_info1c) != PHASE2_INFO1_OK)
		return V34_PHASE2_INVALID_STATE;
	next = *phase2;
	next.state.info1 = exchange;
	next.local_info1.info1c = *local_info1c;
	next.local_info1_stored = true;
	next.concurrent_probe_receive = false;
	next.stage = V34_PHASE2_INFO1;
	if (!v34_phase2_is_valid(&next))
		return V34_PHASE2_INVALID_STATE;
	*phase2 = next;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_start_answerer_info1(
	struct v34_phase2 *phase2)
{
	struct v34_phase2_info1_exchange exchange;
	struct v34_phase2 next;

	if (!v34_phase2_is_valid(phase2))
		return V34_PHASE2_INVALID_ARGUMENT;
	if (phase2->local_role != V34_ANSWERING_STATION ||
	    !ready_for_info1(phase2) || !phase2->peer_info0_stored ||
	    !phase2->remote_probe_stored)
		return V34_PHASE2_INVALID_STATE;
	if (v34_phase2_info1_exchange_answerer_init(
		    &exchange, &phase2->state.reciprocal_probe) !=
	    PHASE2_INFO1_OK)
		return V34_PHASE2_INVALID_STATE;
	next = *phase2;
	next.state.info1 = exchange;
	next.local_info1 = (union v34_phase2_info1_parameters){ 0 };
	next.local_info1_stored = false;
	next.concurrent_probe_receive = false;
	next.stage = V34_PHASE2_INFO1;
	if (!v34_phase2_is_valid(&next))
		return V34_PHASE2_INVALID_STATE;
	*phase2 = next;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_peer_info1c(
	const struct v34_phase2 *phase2, struct v34_info1c *peer_info1c)
{
	if (!v34_phase2_is_valid(phase2) || peer_info1c == NULL)
		return V34_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != V34_PHASE2_FAILURE_NONE ||
	    phase2->stage != V34_PHASE2_INFO1 ||
	    phase2->local_role != V34_ANSWERING_STATION ||
	    !phase2->state.info1.peer_parameters_received ||
	    !info1_records_are_consistent(phase2))
		return V34_PHASE2_INVALID_STATE;
	*peer_info1c = phase2->state.info1.peer_parameters.info1c;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_answerer_reply(
	struct v34_phase2 *phase2, const struct v34_info1a *local_info1a)
{
	struct v34_phase2 next;
	const struct v34_info1c *peer_info1c;

	if (!v34_phase2_is_valid(phase2) ||
	    !v34_info1a_is_valid(local_info1a))
		return V34_PHASE2_INVALID_ARGUMENT;
	if (phase2->failure != V34_PHASE2_FAILURE_NONE ||
	    phase2->stage != V34_PHASE2_INFO1 ||
	    phase2->local_role != V34_ANSWERING_STATION ||
	    phase2->state.info1.control.phase !=
		    PHASE2_INFO1_SIGNAL_A_REPLY_NEEDED)
		return V34_PHASE2_INVALID_STATE;
	peer_info1c = &phase2->state.info1.peer_parameters.info1c;
	if (!v34_phase2_info1a_is_consistent(
		    caller_info0(phase2), answerer_info0(phase2),
		    peer_info1c, local_info1a))
		return V34_PHASE2_INVALID_ARGUMENT;
	next = *phase2;
	if (v34_phase2_info1_exchange_answerer_reply(
		    &next.state.info1, local_info1a) !=
	    PHASE2_INFO1_OK)
		return V34_PHASE2_INVALID_STATE;
	next.local_info1.info1a = *local_info1a;
	next.local_info1_stored = true;
	if (!v34_phase2_is_valid(&next))
		return V34_PHASE2_INVALID_STATE;
	*phase2 = next;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_profiles(
	const struct v34_phase2 *phase2,
	struct v34_phase3_profile *local_transmit,
	struct v34_phase3_profile *peer_transmit)
{
	struct v34_phase3_profile caller;
	struct v34_phase3_profile answerer;
	const struct v34_info1c *info1c;
	const struct v34_info1a *info1a;

	if (!v34_phase2_is_valid(phase2) || local_transmit == NULL ||
	    peer_transmit == NULL || local_transmit == peer_transmit)
		return V34_PHASE2_INVALID_ARGUMENT;
	if (!ready_for_phase3(phase2))
		return V34_PHASE2_INVALID_STATE;
	info1c = phase2->local_role == V34_CALLING_STATION ?
			 &phase2->local_info1.info1c :
			 &phase2->state.info1.peer_parameters.info1c;
	info1a = phase2->local_role == V34_ANSWERING_STATION ?
			 &phase2->local_info1.info1a :
			 &phase2->state.info1.peer_parameters.info1a;
	if (v34_phase2_select_profiles(
		    caller_info0(phase2), answerer_info0(phase2), info1c,
		    info1a, &caller, &answerer) != V34_PHASE2_SELECTION_OK)
		return V34_PHASE2_INVALID_STATE;
	if (phase2->local_role == V34_CALLING_STATION) {
		*local_transmit = caller;
		*peer_transmit = answerer;
	} else {
		*local_transmit = answerer;
		*peer_transmit = caller;
	}
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
	const struct v34_phase2_info0_exchange *exchange =
		&phase2->state.ranging.info0_exchange;
	uint16_t round_trip_delay =
		phase2->state.ranging.control.round_trip_delay_samples;
	bool receive_concurrent = phase2->concurrent_probe_receive;

	if (!exchange->peer_info0_received ||
	    !v34_info0_is_valid(&exchange->peer_info0))
		return false;
	/* A physical caller may begin its probe just before the answerer's local
	 * probe boundary. Search exactly one synchronization capture in that
	 * interval when the measured echo cannot yet have returned. A failed
	 * initial-call trial disables itself; retrain recovery retains its existing
	 * repeated-search behavior. */
	if (phase2->local_role == V34_ANSWERING_STATION &&
	    phase2->prior_round_trip_delay_samples == 0 &&
	    round_trip_delay >=
		    V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES)
		receive_concurrent = true;
	if (v34_phase2_initial_probe_init(
		    &next, &phase2->state.ranging,
		    receive_concurrent) !=
	    V34_PHASE2_INITIAL_PROBE_OK)
		return false;
	phase2->concurrent_probe_receive = receive_concurrent;
	phase2->peer_info0 = exchange->peer_info0;
	phase2->peer_info0_stored = true;
	phase2->state.initial_probe = next;
	phase2->stage = V34_PHASE2_INITIAL_PROBE;
	event->kind = V34_PHASE2_EVENT_INITIAL_PROBE_STARTED;
	event->round_trip_delay_samples =
		next.ranging.control.round_trip_delay_samples;
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
	const struct v34_phase2_initial_probe *initial =
		&phase2->state.turnaround.initial_probe;
	struct v34_phase2_probe_receiver_measurement measurement;
	int16_t receive_gain = 0;
	bool retain_concurrent = false;

	if (initial->concurrent_receive_enabled &&
	    initial->concurrent_receive.phase ==
		    V34_PHASE2_PROBE_ACQUISITION_COMPLETE &&
	    !phase2->remote_probe_stored) {
		if (v34_phase2_initial_probe_measure(
			    initial, &measurement) !=
			    V34_PHASE2_INITIAL_PROBE_OK ||
		    v34_phase2_initial_probe_receive_gain(
			    initial, &receive_gain) !=
			    V34_PHASE2_INITIAL_PROBE_OK ||
		    !v34_phase2_probe_measurement_is_valid(&measurement) ||
		    receive_gain <= 0)
			return false;
		retain_concurrent = true;
	}

	if (v34_phase2_reciprocal_probe_init(
		    &next, &phase2->state.turnaround) !=
	    PHASE2_RECIPROCAL_PROBE_OK)
		return false;
	if (retain_concurrent) {
		phase2->remote_probe = measurement;
		phase2->remote_receive_gain = receive_gain;
		phase2->remote_probe_stored = true;
	}
	phase2->state.reciprocal_probe = next;
	phase2->stage = V34_PHASE2_RECIPROCAL_PROBE;
	event->kind = V34_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED;
	event->round_trip_delay_samples = next.turnaround.initial_probe.ranging
		.control.round_trip_delay_samples;
	return true;
}

static bool sample_ranging(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_ranging_event child;
	uint32_t measured;
	uint32_t prior;
	uint32_t separation;

	if (v34_phase2_ranging_sample(
		    &phase2->state.ranging, received_sample,
		    transmit_sample, &child) != V34_PHASE2_RANGING_OK)
		return false;
	if (child.exchange_event.peer_info0_event ==
	    V34_PHASE2_PEER_INFO0_RECEIVED) {
		phase2->peer_info0 = child.exchange_event.peer_info0;
		phase2->peer_info0_stored = true;
	}
	/* Each line terminal turns a ranging reversal around in 40 ms. A larger
	 * within-call RTD change is an early or late reversal alias, not a new
	 * propagation delay. Preserve the confirmed RTD before it sizes the probe. */
	measured = child.round_trip_delay_samples;
	prior = phase2->prior_round_trip_delay_samples;
	separation = measured > prior ? measured - prior : prior - measured;
	if (prior != 0u &&
	    phase2->state.ranging.control.round_trip_delay_valid &&
	    (child.kind == PHASE2_ROUND_TRIP_DELAY_MEASURED ||
	     child.kind == PHASE2_PROBE_RECEIVE_READY ||
	     child.kind == PHASE2_PROBE_TRANSMIT_READY) &&
	    separation > PHASE2_RANGING_TURNAROUND_SAMPLES) {
		/* Either alias can leave the caller probing before our own probe.
		 * Retain that measurement instead of waiting for it a second time. */
		if (phase2->local_role == V34_ANSWERING_STATION)
			phase2->concurrent_probe_receive = true;
		child.round_trip_delay_samples = prior;
		phase2->state.ranging.control.round_trip_delay_samples = prior;
	}
	switch (child.kind) {
	case PHASE2_ROUND_TRIP_DELAY_MEASURED:
		event->kind = V34_PHASE2_EVENT_ROUND_TRIP_DELAY;
		event->round_trip_delay_samples =
			child.round_trip_delay_samples;
		break;
	case PHASE2_PROBE_RECEIVE_READY:
	case PHASE2_PROBE_TRANSMIT_READY:
		return enter_initial_probe(phase2, event);
	case PHASE2_RANGING_NO_EVENT:
		break;
	}
	return true;
}

static bool sample_initial_probe(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_initial_probe_event child;
	enum v34_station_role role = phase2->local_role;
	bool measured_remote_probe;

	if (v34_phase2_initial_probe_sample(
		    &phase2->state.initial_probe, received_sample,
		    transmit_sample, &child) != V34_PHASE2_INITIAL_PROBE_OK)
		return false;
	phase2->concurrent_probe_receive =
		phase2->state.initial_probe.concurrent_receive_enabled;
	measured_remote_probe =
		child.kind == PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE ||
		(role == V34_ANSWERING_STATION &&
		 child.receive_event.kind ==
			 V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED);
	if (child.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED) {
		if (child.failure == PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT) {
			struct v34_phase2_ranging next =
				phase2->state.initial_probe.ranging;

			if (phase2_ranging_restart_after_probe(
				    &next.control, &next.info0_exchange.transport) !=
			    PHASE2_RANGING_OK)
				return false;
			next.recovery_control = (struct phase2_ranging_control){ 0 };
			next.recovery_paused = false;
			phase2->state.ranging = next;
			phase2->stage = V34_PHASE2_RANGING;
			phase2->concurrent_probe_receive = false;
			phase2->remote_probe_stored = false;
			phase2->remote_receive_gain = 0;
			return true;
		}
		phase2->failure = initial_probe_failure(child.failure);
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
	} else if (measured_remote_probe) {
		struct v34_phase2_probe_receiver_measurement measurement;
		int16_t receive_gain;

		if (v34_phase2_initial_probe_measure(
			    &phase2->state.initial_probe,
			    &measurement) !=
		    V34_PHASE2_INITIAL_PROBE_OK ||
		    v34_phase2_initial_probe_receive_gain(
			    &phase2->state.initial_probe, &receive_gain) !=
			    V34_PHASE2_INITIAL_PROBE_OK ||
		    !v34_phase2_probe_measurement_is_valid(
			    &measurement) ||
		    receive_gain <= 0)
			return false;
		phase2->remote_probe = measurement;
		phase2->remote_receive_gain = receive_gain;
		phase2->remote_probe_stored = true;
		if (role == V34_CALLING_STATION && !enter_turnaround(phase2, event))
			return false;
		event->kind =
			V34_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED;
	}
	if (phase2->failure == V34_PHASE2_FAILURE_NONE &&
	    role == V34_ANSWERING_STATION &&
	    child.kind == PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE) {
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
	phase2->concurrent_probe_receive =
		phase2->state.turnaround.initial_probe
			.concurrent_receive_enabled;
	if (child.kind == PHASE2_PROBE_TURNAROUND_EVENT_FAILED) {
		phase2->failure = V34_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT;
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
	} else if (child.kind ==
		   PHASE2_PROBE_TURNAROUND_EVENT_RECOVERY_STARTED) {
		event->kind =
			V34_PHASE2_EVENT_PROBE_TURNAROUND_RECOVERY_STARTED;
	} else if (child.kind ==
			   PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY ||
		   child.kind ==
			   PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY) {
		return enter_reciprocal_probe(phase2, event);
	}
	return true;
}

static bool sample_reciprocal_probe(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct phase2_reciprocal_probe_event child;

	if (v34_phase2_reciprocal_probe_sample(
		    &phase2->state.reciprocal_probe, received_sample,
		    transmit_sample, &child) !=
	    PHASE2_RECIPROCAL_PROBE_OK)
		return false;
	if (child.kind == PHASE2_RECIPROCAL_PROBE_EVENT_FAILED) {
		phase2->failure = child.failure ==
					  PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND ?
				  V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND :
				  V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT;
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
	} else if (child.kind ==
		   PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED) {
		struct v34_phase2_probe_receiver_measurement measurement;
		int16_t receive_gain;

		if (v34_phase2_reciprocal_probe_measure(
			    &phase2->state.reciprocal_probe,
			    &measurement) !=
		    PHASE2_RECIPROCAL_PROBE_OK ||
		    v34_phase2_reciprocal_probe_receive_gain(
			    &phase2->state.reciprocal_probe,
			    &receive_gain) !=
			    PHASE2_RECIPROCAL_PROBE_OK ||
		    !v34_phase2_probe_measurement_is_valid(
			    &measurement) ||
		    receive_gain <= 0)
			return false;
		phase2->remote_probe = measurement;
		phase2->remote_receive_gain = receive_gain;
		phase2->remote_probe_stored = true;
		event->kind = V34_PHASE2_EVENT_READY_FOR_INFO1;
	} else if (child.kind ==
		   PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED) {
		if (!phase2->remote_probe_stored)
			return false;
		event->kind = V34_PHASE2_EVENT_READY_FOR_INFO1;
	}
	return true;
}

static bool sample_info1(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event)
{
	struct v34_phase2_info1_exchange_event child;

	if (v34_phase2_info1_exchange_sample(
		    &phase2->state.info1, received_sample,
		    transmit_sample, &child) !=
	    PHASE2_INFO1_OK)
		return false;
	switch (child.kind) {
	case V34_PHASE2_INFO1_EVENT_NONE:
		break;
	case V34_PHASE2_INFO1_EVENT_LOCAL_INFO1C_SENT:
		event->kind = V34_PHASE2_EVENT_LOCAL_INFO1C_SENT;
		break;
	case V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED:
		event->kind = V34_PHASE2_EVENT_PEER_INFO1C_RECEIVED;
		break;
	case V34_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT:
		event->kind = V34_PHASE2_EVENT_LOCAL_INFO1A_SENT;
		break;
	case V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED:
		event->kind = V34_PHASE2_EVENT_PEER_INFO1A_RECEIVED;
		break;
	case V34_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED:
		event->kind = V34_PHASE2_EVENT_PEER_INFO1_REJECTED;
		break;
	case V34_PHASE2_INFO1_EVENT_FAILED:
		if (child.failure != PHASE2_INFO1_PEER_TIMEOUT ||
		    phase2_info1_recover(&phase2->state.info1.control) !=
			    PHASE2_INFO1_OK)
			return false;
		event->kind = V34_PHASE2_EVENT_INFO1_RECOVERY_STARTED;
		break;
	}
	if ((child.kind == V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED ||
	     child.kind == V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) &&
	    !info1_records_are_consistent(phase2)) {
		phase2->failure = V34_PHASE2_FAILURE_INCONSISTENT_INFO1;
		event->kind = V34_PHASE2_EVENT_FAILED;
		event->failure = phase2->failure;
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
	case V34_PHASE2_INFO1:
		accepted = sample_info1(
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
	if (!phase2->remote_probe_stored ||
	    (!ready_for_info1(phase2) && phase2->stage != V34_PHASE2_INFO1))
		return V34_PHASE2_INVALID_STATE;
	*measurement = phase2->remote_probe;
	return V34_PHASE2_OK;
}

enum v34_phase2_result v34_phase2_receive_gain(
	const struct v34_phase2 *phase2, int16_t *receive_gain)
{
	if (!v34_phase2_is_valid(phase2) || receive_gain == NULL)
		return V34_PHASE2_INVALID_ARGUMENT;
	if (!phase2->remote_probe_stored ||
	    (!ready_for_info1(phase2) && phase2->stage != V34_PHASE2_INFO1))
		return V34_PHASE2_INVALID_STATE;
	*receive_gain = phase2->remote_receive_gain;
	return V34_PHASE2_OK;
}
