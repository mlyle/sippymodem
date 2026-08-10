// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase2_probe_turnaround.h"

#include <stddef.h>

_Static_assert(
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES <=
		UINT16_MAX,
	"Probe-turnaround deadline must fit its sample counter");

static enum sippymodem_v34_phase2_station_role local_role(
	const struct sippymodem_v34_phase2_probe_turnaround *turnaround)
{
	return turnaround->initial_probe.ranging.info0_exchange.local_role;
}

static bool completed_probe_matches_role(
	const struct sippymodem_v34_phase2_initial_probe *probe)
{
	enum sippymodem_v34_phase2_station_role role =
		probe->ranging.info0_exchange.local_role;

	return role == SIPPYMODEM_V34_PHASE2_CALLING_STATION ?
		       probe->phase ==
			       SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE :
		       probe->phase ==
			       SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A;
}

static bool caller_phase_is_valid(
	const struct sippymodem_v34_phase2_probe_turnaround *turnaround)
{
	switch (turnaround->phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples <
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY:
		return false;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples ==
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	}
	return false;
}

static bool answerer_phase_is_valid(
	const struct sippymodem_v34_phase2_probe_turnaround *turnaround)
{
	switch (turnaround->phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples <
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL:
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY:
		return false;
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples ==
			       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	}
	return false;
}

bool sippymodem_v34_phase2_probe_turnaround_is_valid(
	const struct sippymodem_v34_phase2_probe_turnaround *turnaround)
{
	bool timed_out;

	if (turnaround == NULL ||
	    !sippymodem_v34_phase2_initial_probe_is_valid(
		    &turnaround->initial_probe) ||
	    !completed_probe_matches_role(&turnaround->initial_probe) ||
	    (unsigned int)turnaround->phase >
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT ||
	    (unsigned int)turnaround->failure >
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT)
		return false;
	timed_out = turnaround->phase ==
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	if (timed_out !=
	    (turnaround->failure ==
	     SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT))
		return false;
	return local_role(turnaround) ==
		       SIPPYMODEM_V34_PHASE2_CALLING_STATION ?
		       caller_phase_is_valid(turnaround) :
		       answerer_phase_is_valid(turnaround);
}

enum sippymodem_v34_phase2_probe_turnaround_result
sippymodem_v34_phase2_probe_turnaround_init(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	const struct sippymodem_v34_phase2_initial_probe *completed_probe)
{
	struct sippymodem_v34_phase2_probe_turnaround initialized = { 0 };

	if (turnaround == NULL ||
	    !sippymodem_v34_phase2_initial_probe_is_valid(completed_probe) ||
	    !completed_probe_matches_role(completed_probe))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	initialized.initial_probe = *completed_probe;
	if (completed_probe->ranging.info0_exchange.local_role ==
	    SIPPYMODEM_V34_PHASE2_CALLING_STATION) {
		initialized.phase =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL;
	} else {
		initialized.phase =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A;
		initialized.phase_samples_remaining =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES;
	}
	if (!sippymodem_v34_phase2_probe_turnaround_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = initialized;
	return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_OK;
}

static bool observe_remote_tone(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	return sippymodem_v34_phase2_tone_detector_sample(
		       &turnaround->initial_probe.ranging.info0_exchange.tone_detector,
		       received_sample, &event->tone_event) ==
	       SIPPYMODEM_V34_PHASE2_TONE_OK;
}

static bool generate_local_tone(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	int16_t *sample)
{
	return sippymodem_v34_phase2_tone_transmitter_sample(
		       &turnaround->initial_probe.ranging.info0_exchange
			.tone_transmitter,
		       sample) == SIPPYMODEM_V34_PHASE2_TONE_OK;
}

static bool reverse_local_tone(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround)
{
	return sippymodem_v34_phase2_tone_transmitter_reverse(
		       &turnaround->initial_probe.ranging.info0_exchange
			.tone_transmitter) == SIPPYMODEM_V34_PHASE2_TONE_OK;
}

static bool phase_transmits_tone(
	enum sippymodem_v34_phase2_probe_turnaround_phase phase)
{
	return phase <=
		       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL ||
	       phase ==
		       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A ||
	       phase ==
		       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL;
}

static bool prepare_caller_response(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround)
{
	if (turnaround->phase !=
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY)
		return true;
	if (--turnaround->phase_samples_remaining != 0)
		return true;
	turnaround->phase =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL;
	turnaround->phase_samples_remaining =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES;
	return reverse_local_tone(turnaround);
}

static void enter_ready(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	turnaround->phase_samples_remaining = 0;
	turnaround->wait_samples = 0;
	if (local_role(turnaround) == SIPPYMODEM_V34_PHASE2_CALLING_STATION) {
		turnaround->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY;
	} else {
		turnaround->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;
	}
}

static bool finish_fixed_interval(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	if (turnaround->phase !=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL &&
	    turnaround->phase !=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A &&
	    turnaround->phase !=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL)
		return true;
	if (--turnaround->phase_samples_remaining != 0)
		return true;
	if (turnaround->phase ==
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL) {
		enter_ready(turnaround, event);
		return true;
	}
	if (turnaround->phase ==
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A) {
		turnaround->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL;
		turnaround->phase_samples_remaining =
			SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES;
		return reverse_local_tone(turnaround);
	}
	turnaround->phase =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL;
	return true;
}

static void schedule_caller_response(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples)
{
	if (detector_delay_samples == 0 ||
	    detector_delay_samples >=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES)
		return;
	turnaround->phase =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY;
	turnaround->phase_samples_remaining =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES -
		detector_delay_samples;
	turnaround->wait_samples = 0;
}

static void schedule_answerer_probe(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	turnaround->wait_samples = 0;
	if (detector_delay_samples >=
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES) {
		enter_ready(turnaround, event);
		return;
	}
	turnaround->phase =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START;
	turnaround->phase_samples_remaining =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
		detector_delay_samples;
}

static void handle_remote_reversal(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	if (turnaround->phase ==
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL)
		schedule_caller_response(turnaround, detector_delay_samples);
	else if (turnaround->phase ==
		 SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL)
		schedule_answerer_probe(
			turnaround, detector_delay_samples, event);
}

static void advance_answerer_probe_start(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	if (turnaround->phase !=
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START)
		return;
	/* This call already consumed one sample after the estimated reversal.
	 * Signal readiness on the final expected tail sample. */
	if (--turnaround->phase_samples_remaining == 0)
		enter_ready(turnaround, event);
}

static bool phase_waits_for_reversal(
	enum sippymodem_v34_phase2_probe_turnaround_phase phase)
{
	return phase ==
		       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL ||
	       phase ==
		       SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL;
}

static void advance_reversal_wait(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	if (!phase_waits_for_reversal(turnaround->phase))
		return;
	if (++turnaround->wait_samples <
	    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES)
		return;
	turnaround->phase = SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	turnaround->failure =
		SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT;
	turnaround->phase_samples_remaining = 0;
	event->kind = SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED;
	event->failure = turnaround->failure;
}

enum sippymodem_v34_phase2_probe_turnaround_result
sippymodem_v34_phase2_probe_turnaround_sample(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_probe_turnaround_event *event)
{
	struct sippymodem_v34_phase2_probe_turnaround next;
	struct sippymodem_v34_phase2_probe_turnaround_event emitted = { 0 };
	int16_t generated = 0;

	if (!sippymodem_v34_phase2_probe_turnaround_is_valid(turnaround) ||
	    transmit_sample == NULL || event == NULL)
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	next = *turnaround;
	if (!prepare_caller_response(&next))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (next.phase !=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY &&
	    next.phase !=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY &&
	    next.phase != SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	    !observe_remote_tone(&next, received_sample, &emitted))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (phase_transmits_tone(next.phase) &&
	    !generate_local_tone(&next, &generated))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (emitted.tone_event.kind ==
	    SIPPYMODEM_V34_PHASE2_TONE_PHASE_REVERSED)
		handle_remote_reversal(
			&next, emitted.tone_event.reversal_delay_samples, &emitted);
	advance_reversal_wait(&next, &emitted);
	if (!finish_fixed_interval(&next, &emitted))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	advance_answerer_probe_start(&next, &emitted);
	if (!sippymodem_v34_phase2_probe_turnaround_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = next;
	*transmit_sample = generated;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_OK;
}

const char *sippymodem_v34_phase2_probe_turnaround_result_name(
	enum sippymodem_v34_phase2_probe_turnaround_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
