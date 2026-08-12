// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_turnaround.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES <=
		UINT16_MAX,
	"Probe-turnaround deadline must fit its sample counter");
_Static_assert(
	V34_PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES > 0 &&
	(int)V34_PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES <
		(int)V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES,
	"Probe reception must begin within the reversed-tone tail");

static enum v34_station_role local_role(
	const struct v34_phase2_probe_turnaround *turnaround)
{
	return turnaround->initial_probe.ranging.info0_exchange.local_role;
}

static bool completed_probe_matches_role(
	const struct v34_phase2_initial_probe *probe)
{
	enum v34_station_role role =
		probe->ranging.info0_exchange.local_role;

	return role == V34_CALLING_STATION ?
		       probe->control.phase ==
			       PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE :
		       probe->control.phase ==
			       PHASE2_INITIAL_PROBE_SIGNAL_A_TONE;
}

static bool caller_phase_is_valid(
	const struct v34_phase2_probe_turnaround *turnaround)
{
	switch (turnaround->phase) {
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples <
			       V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <
			       V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES &&
		       turnaround->wait_samples == 0;
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0;
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples == 0;
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A:
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL:
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL:
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START:
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY:
		return false;
	case V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples ==
			       V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	}
	return false;
}

static bool answerer_phase_is_valid(
	const struct v34_phase2_probe_turnaround *turnaround)
{
	bool info1c_epoch_started =
		turnaround->answerer_info1c_elapsed_samples != 0;

	switch (turnaround->phase) {
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
		       turnaround->wait_samples == 0 && !info1c_epoch_started;
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0 && !info1c_epoch_started;
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples <
			       V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
		       !info1c_epoch_started;
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <
			       V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0 && info1c_epoch_started;
	case V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples == 0 && info1c_epoch_started;
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL:
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY:
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL:
	case V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY:
		return false;
	case V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples ==
			       V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
		       !info1c_epoch_started;
	}
	return false;
}

bool v34_phase2_probe_turnaround_is_valid(
	const struct v34_phase2_probe_turnaround *turnaround)
{
	bool timed_out;

	if (turnaround == NULL ||
	    !v34_phase2_initial_probe_is_valid(
		    &turnaround->initial_probe) ||
	    !completed_probe_matches_role(&turnaround->initial_probe) ||
	    (unsigned int)turnaround->phase >
		    V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT ||
	    (unsigned int)turnaround->failure >
		    V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT)
		return false;
	if (local_role(turnaround) ==
		    V34_CALLING_STATION &&
	    turnaround->answerer_info1c_elapsed_samples != 0)
		return false;
	timed_out = turnaround->phase ==
		    V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	if (timed_out !=
	    (turnaround->failure ==
	     V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT))
		return false;
	return local_role(turnaround) ==
		       V34_CALLING_STATION ?
		       caller_phase_is_valid(turnaround) :
		       answerer_phase_is_valid(turnaround);
}

enum v34_phase2_probe_turnaround_result
v34_phase2_probe_turnaround_init(
	struct v34_phase2_probe_turnaround *turnaround,
	const struct v34_phase2_initial_probe *completed_probe)
{
	struct v34_phase2_probe_turnaround initialized = { 0 };

	if (turnaround == NULL ||
	    !v34_phase2_initial_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	initialized.initial_probe = *completed_probe;
	if (completed_probe->ranging.info0_exchange.local_role ==
	    V34_CALLING_STATION) {
		initialized.phase =
			V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL;
	} else {
		initialized.phase =
			V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A;
		initialized.phase_samples_remaining =
			V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES;
	}
	if (!v34_phase2_probe_turnaround_is_valid(&initialized))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = initialized;
	return V34_PHASE2_PROBE_TURNAROUND_OK;
}

static bool observe_remote_tone(
	struct v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample,
	struct v34_phase2_probe_turnaround_event *event)
{
	return v34_phase2_tone_detector_sample(
		       &turnaround->initial_probe.ranging.info0_exchange.transport
			.tone_detector,
		       received_sample, &event->tone_event) ==
	       V34_PHASE2_TONE_OK;
}

static bool generate_local_tone(
	struct v34_phase2_probe_turnaround *turnaround,
	int16_t *sample)
{
	return v34_phase2_tone_transmitter_sample(
		       &turnaround->initial_probe.ranging.info0_exchange
			.transport.tone_transmitter,
		       sample) == V34_PHASE2_TONE_OK;
}

static bool reverse_local_tone(
	struct v34_phase2_probe_turnaround *turnaround)
{
	return v34_phase2_tone_transmitter_reverse(
		       &turnaround->initial_probe.ranging.info0_exchange
			.transport.tone_transmitter) == V34_PHASE2_TONE_OK;
}

static bool phase_transmits_tone(
	enum v34_phase2_probe_turnaround_phase phase)
{
	return phase <=
		       V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL ||
	       phase ==
		       V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A ||
	       phase ==
		       V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL;
}

static bool prepare_caller_response(
	struct v34_phase2_probe_turnaround *turnaround)
{
	if (turnaround->phase !=
	    V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY)
		return true;
	if (--turnaround->phase_samples_remaining != 0)
		return true;
	turnaround->phase =
		V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL;
	turnaround->phase_samples_remaining =
		V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES;
	return reverse_local_tone(turnaround);
}

static void enter_ready(
	struct v34_phase2_probe_turnaround *turnaround,
	struct v34_phase2_probe_turnaround_event *event)
{
	turnaround->phase_samples_remaining = 0;
	turnaround->wait_samples = 0;
	if (local_role(turnaround) == V34_CALLING_STATION) {
		turnaround->phase =
			V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY;
		event->kind =
			V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY;
	} else {
		turnaround->phase =
			V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY;
		event->kind =
			V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;
	}
}

static bool finish_fixed_interval(
	struct v34_phase2_probe_turnaround *turnaround,
	struct v34_phase2_probe_turnaround_event *event)
{
	if (turnaround->phase !=
		    V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL &&
	    turnaround->phase !=
		    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A &&
	    turnaround->phase !=
		    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL)
		return true;
	if (--turnaround->phase_samples_remaining != 0)
		return true;
	if (turnaround->phase ==
	    V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL) {
		enter_ready(turnaround, event);
		return true;
	}
	if (turnaround->phase ==
	    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A) {
		turnaround->phase =
			V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL;
		turnaround->phase_samples_remaining =
			V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES;
		return reverse_local_tone(turnaround);
	}
	turnaround->phase =
		V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL;
	return true;
}

static void schedule_caller_response(
	struct v34_phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples)
{
	if (detector_delay_samples == 0 ||
	    detector_delay_samples >=
		    V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES)
		return;
	turnaround->phase =
		V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY;
	turnaround->phase_samples_remaining =
		V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES -
		detector_delay_samples;
	turnaround->wait_samples = 0;
}

static void schedule_answerer_probe(
	struct v34_phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples,
	struct v34_phase2_probe_turnaround_event *event)
{
	uint16_t handoff_delay =
		V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
		V34_PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES;

	turnaround->wait_samples = 0;
	turnaround->answerer_info1c_elapsed_samples = detector_delay_samples;
	if (detector_delay_samples >= handoff_delay) {
		enter_ready(turnaround, event);
		return;
	}
	turnaround->phase =
		V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START;
	turnaround->phase_samples_remaining = handoff_delay -
					      detector_delay_samples;
}

static void handle_remote_reversal(
	struct v34_phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples,
	struct v34_phase2_probe_turnaround_event *event)
{
	if (turnaround->phase ==
	    V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL)
		schedule_caller_response(turnaround, detector_delay_samples);
	else if (turnaround->phase ==
		 V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL)
		schedule_answerer_probe(
			turnaround, detector_delay_samples, event);
}

static void advance_answerer_probe_start(
	struct v34_phase2_probe_turnaround *turnaround,
	struct v34_phase2_probe_turnaround_event *event)
{
	if (turnaround->phase !=
	    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START)
		return;
	/* This call already consumed one sample after the estimated reversal.
	 * Leave the synchronizer's bounded search interval before L1. */
	if (--turnaround->phase_samples_remaining == 0)
		enter_ready(turnaround, event);
}

static bool phase_waits_for_reversal(
	enum v34_phase2_probe_turnaround_phase phase)
{
	return phase ==
		       V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL ||
	       phase ==
		       V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL;
}

static void advance_reversal_wait(
	struct v34_phase2_probe_turnaround *turnaround,
	struct v34_phase2_probe_turnaround_event *event)
{
	if (!phase_waits_for_reversal(turnaround->phase))
		return;
	if (++turnaround->wait_samples <
	    V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES)
		return;
	turnaround->phase = V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	turnaround->failure =
		V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT;
	turnaround->phase_samples_remaining = 0;
	event->kind = V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED;
	event->failure = turnaround->failure;
}

enum v34_phase2_probe_turnaround_result
v34_phase2_probe_turnaround_sample(
	struct v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_probe_turnaround_event *event)
{
	struct v34_phase2_probe_turnaround next;
	struct v34_phase2_probe_turnaround_event emitted = { 0 };
	int16_t generated = 0;
	bool advance_info1c_epoch;

	if (!v34_phase2_probe_turnaround_is_valid(turnaround) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	next = *turnaround;
	advance_info1c_epoch =
		next.answerer_info1c_elapsed_samples != 0 &&
		next.phase !=
			V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY;
	if (!prepare_caller_response(&next))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (next.phase !=
		    V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY &&
	    next.phase !=
		    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY &&
	    next.phase != V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	    !observe_remote_tone(&next, received_sample, &emitted))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (phase_transmits_tone(next.phase) &&
	    !generate_local_tone(&next, &generated))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (emitted.tone_event.kind ==
	    V34_PHASE2_TONE_PHASE_REVERSED)
		handle_remote_reversal(
			&next, emitted.tone_event.reversal_delay_samples, &emitted);
	advance_reversal_wait(&next, &emitted);
	if (!finish_fixed_interval(&next, &emitted))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	advance_answerer_probe_start(&next, &emitted);
	if (advance_info1c_epoch)
		next.answerer_info1c_elapsed_samples++;
	if (!v34_phase2_probe_turnaround_is_valid(&next))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_PROBE_TURNAROUND_OK;
}

const char *v34_phase2_probe_turnaround_result_name(
	enum v34_phase2_probe_turnaround_result result)
{
	switch (result) {
	case V34_PHASE2_PROBE_TURNAROUND_OK:
		return "ok";
	case V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
