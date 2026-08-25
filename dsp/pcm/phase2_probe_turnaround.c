// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_probe_turnaround.h"

#include <stddef.h>

_Static_assert(
	PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES <=
		UINT16_MAX,
	"Probe-turnaround deadline must fit its sample counter");
_Static_assert(
	PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES > 0 &&
	(int)PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES <
		(int)PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES,
	"Probe reception must begin within the reversed-tone tail");

static bool signal_is_b(const struct phase2_info0_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B;
}

static bool completed_probe_matches_role(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange)
{
	return signal_is_b(exchange) ?
		       probe->phase ==
			       PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE :
		       probe->phase ==
			       PHASE2_INITIAL_PROBE_SIGNAL_A_TONE;
}

static bool signal_b_phase_is_valid(
	const struct phase2_probe_turnaround *turnaround)
{
	switch (turnaround->phase) {
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples <
			       PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <
			       PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES &&
		       turnaround->wait_samples == 0;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples == 0;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_B_REVERSAL:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_PROBE_START:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY:
		return false;
	case PHASE2_PROBE_TURNAROUND_TIMED_OUT:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples ==
			       PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	}
	return false;
}

static bool signal_a_phase_is_valid(
	const struct phase2_probe_turnaround *turnaround)
{
	bool reversal_epoch_started =
		turnaround->signal_a_reversal_elapsed_samples != 0;

	switch (turnaround->phase) {
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
		       turnaround->wait_samples == 0 && !reversal_epoch_started;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <=
			       PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0 && !reversal_epoch_started;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_B_REVERSAL:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples <
			       PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
		       !reversal_epoch_started;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_PROBE_START:
		return turnaround->phase_samples_remaining > 0 &&
		       turnaround->phase_samples_remaining <
			       PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
		       turnaround->wait_samples == 0 && reversal_epoch_started;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples == 0 && reversal_epoch_started;
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL:
	case PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY:
		return false;
	case PHASE2_PROBE_TURNAROUND_TIMED_OUT:
		return turnaround->phase_samples_remaining == 0 &&
		       turnaround->wait_samples ==
			       PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
		       !reversal_epoch_started;
	}
	return false;
}

bool phase2_probe_turnaround_is_valid(
	const struct phase2_probe_turnaround *turnaround,
	const struct phase2_initial_probe *completed_probe,
	const struct phase2_info0_exchange *exchange)
{
	bool timed_out;

	if (turnaround == NULL ||
	    !phase2_initial_probe_is_valid(completed_probe, exchange) ||
	    !completed_probe_matches_role(completed_probe, exchange) ||
	    (unsigned int)turnaround->phase >
		    PHASE2_PROBE_TURNAROUND_TIMED_OUT ||
	    (unsigned int)turnaround->failure >
		    PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT)
		return false;
	if (signal_is_b(exchange) &&
	    turnaround->signal_a_reversal_elapsed_samples != 0)
		return false;
	timed_out = turnaround->phase ==
		    PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	if (timed_out !=
	    (turnaround->failure ==
	     PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT))
		return false;
	return signal_is_b(exchange) ?
		       signal_b_phase_is_valid(turnaround) :
		       signal_a_phase_is_valid(turnaround);
}

enum phase2_probe_turnaround_result
phase2_probe_turnaround_init(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_initial_probe *completed_probe,
	const struct phase2_info0_exchange *exchange)
{
	struct phase2_probe_turnaround initialized = { 0 };

	if (turnaround == NULL ||
	    !phase2_initial_probe_is_valid(completed_probe, exchange) ||
	    !completed_probe_matches_role(completed_probe, exchange))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (signal_is_b(exchange)) {
		initialized.phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL;
	} else {
		initialized.phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE;
		initialized.phase_samples_remaining =
			PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES;
	}
	if (!phase2_probe_turnaround_is_valid(
		    &initialized, completed_probe, exchange))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = initialized;
	return PHASE2_PROBE_TURNAROUND_OK;
}

static bool observe_remote_tone(
	struct phase2_info0_exchange *exchange,
	int16_t received_sample,
	struct phase2_probe_turnaround_event *event)
{
	return v34_phase2_tone_detector_sample(
		       &exchange->tone_detector,
		       received_sample, &event->tone_event) ==
	       V34_PHASE2_TONE_OK;
}

static bool generate_local_tone(
	struct phase2_info0_exchange *exchange,
	int16_t *sample)
{
	return v34_phase2_tone_transmitter_sample(
		       &exchange->tone_transmitter, sample) ==
	       V34_PHASE2_TONE_OK;
}

static bool reverse_local_tone(
	struct phase2_info0_exchange *exchange)
{
	return v34_phase2_tone_transmitter_reverse(
		       &exchange->tone_transmitter) == V34_PHASE2_TONE_OK;
}

static bool phase_transmits_tone(
	enum phase2_probe_turnaround_phase phase)
{
	return phase <=
		       PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL ||
	       phase ==
		       PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE ||
	       phase ==
		       PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL;
}

static bool prepare_signal_b_response(
	struct phase2_probe_turnaround *turnaround,
	struct phase2_info0_exchange *exchange)
{
	if (turnaround->phase !=
	    PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY)
		return true;
	if (--turnaround->phase_samples_remaining != 0)
		return true;
	turnaround->phase =
		PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL;
	turnaround->phase_samples_remaining =
		PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES;
	return reverse_local_tone(exchange);
}

static void enter_ready(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_info0_exchange *exchange,
	struct phase2_probe_turnaround_event *event)
{
	turnaround->phase_samples_remaining = 0;
	turnaround->wait_samples = 0;
	if (signal_is_b(exchange)) {
		turnaround->phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY;
		event->kind =
			PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY;
	} else {
		turnaround->phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY;
		event->kind =
			PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;
	}
}

static bool finish_fixed_interval(
	struct phase2_probe_turnaround *turnaround,
	struct phase2_info0_exchange *exchange,
	struct phase2_probe_turnaround_event *event)
{
	if (turnaround->phase !=
		    PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL &&
	    turnaround->phase !=
		    PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE &&
	    turnaround->phase !=
		    PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL)
		return true;
	if (--turnaround->phase_samples_remaining != 0)
		return true;
	if (turnaround->phase ==
	    PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL) {
		enter_ready(turnaround, exchange, event);
		return true;
	}
	if (turnaround->phase ==
	    PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE) {
		turnaround->phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL;
		turnaround->phase_samples_remaining =
			PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES;
		return reverse_local_tone(exchange);
	}
	turnaround->phase =
		PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_B_REVERSAL;
	return true;
}

static void schedule_signal_b_response(
	struct phase2_probe_turnaround *turnaround,
	uint16_t detector_delay_samples)
{
	if (detector_delay_samples == 0 ||
	    detector_delay_samples >=
		    PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES)
		return;
	turnaround->phase =
		PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY;
	turnaround->phase_samples_remaining =
		PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES -
		detector_delay_samples;
	turnaround->wait_samples = 0;
}

static void schedule_signal_a_probe(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_info0_exchange *exchange,
	uint16_t detector_delay_samples,
	struct phase2_probe_turnaround_event *event)
{
	uint16_t handoff_delay =
		PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
		PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES;

	turnaround->wait_samples = 0;
	turnaround->signal_a_reversal_elapsed_samples = detector_delay_samples;
	if (detector_delay_samples >= handoff_delay) {
		enter_ready(turnaround, exchange, event);
		return;
	}
	turnaround->phase =
		PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_PROBE_START;
	turnaround->phase_samples_remaining = handoff_delay -
					      detector_delay_samples;
}

static void handle_remote_reversal(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_info0_exchange *exchange,
	uint16_t detector_delay_samples,
	struct phase2_probe_turnaround_event *event)
{
	if (turnaround->phase ==
	    PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL)
		schedule_signal_b_response(turnaround, detector_delay_samples);
	else if (turnaround->phase ==
		 PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_B_REVERSAL)
		schedule_signal_a_probe(
			turnaround, exchange, detector_delay_samples, event);
}

static void advance_signal_a_probe_start(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_info0_exchange *exchange,
	struct phase2_probe_turnaround_event *event)
{
	if (turnaround->phase !=
	    PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_PROBE_START)
		return;
	/* This call already consumed one sample after the estimated reversal.
	 * Leave the synchronizer's bounded search interval before L1. */
	if (--turnaround->phase_samples_remaining == 0)
		enter_ready(turnaround, exchange, event);
}

static bool phase_waits_for_reversal(
	enum phase2_probe_turnaround_phase phase)
{
	return phase ==
		       PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL ||
	       phase ==
		       PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_B_REVERSAL;
}

static void advance_reversal_wait(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_info0_exchange *exchange,
	struct phase2_probe_turnaround_event *event)
{
	struct v34_phase2_tone_measurement tone;

	if (!phase_waits_for_reversal(turnaround->phase))
		return;
	if (++turnaround->wait_samples <
	    PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES)
		return;
	/* V.34 11.2.2.2.2 returns an answerer which missed the final Tone-B
	 * reversal to 11.2.1.2.3.  The detector has already reacquired a
	 * continuously present Tone B, so repeat the 50-ms Tone-A turn without
	 * discarding its phase reference.  A missing peer tone remains a bounded
	 * terminal failure. */
	tone = v34_phase2_tone_detector_measure(&exchange->tone_detector);
	if (!signal_is_b(exchange) && tone.ready && tone.tone_present) {
		turnaround->phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE;
		turnaround->phase_samples_remaining =
			PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES;
		turnaround->wait_samples = 0;
		event->kind =
			PHASE2_PROBE_TURNAROUND_EVENT_RECOVERY_STARTED;
		return;
	}
	turnaround->phase = PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	turnaround->failure =
		PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT;
	turnaround->phase_samples_remaining = 0;
	event->kind = PHASE2_PROBE_TURNAROUND_EVENT_FAILED;
	event->failure = turnaround->failure;
}

enum phase2_probe_turnaround_result
phase2_probe_turnaround_sample(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_initial_probe *completed_probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_probe_turnaround_event *event)
{
	struct phase2_probe_turnaround next;
	struct phase2_info0_exchange next_exchange;
	struct phase2_probe_turnaround_event emitted = { 0 };
	int16_t generated = 0;
	bool advance_signal_a_epoch;

	if (!phase2_probe_turnaround_is_valid(
		    turnaround, completed_probe, exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	next = *turnaround;
	next_exchange = *exchange;
	advance_signal_a_epoch =
		next.signal_a_reversal_elapsed_samples != 0 &&
		next.phase !=
			PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY;
	if (!prepare_signal_b_response(&next, &next_exchange))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (next.phase !=
		    PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY &&
	    next.phase !=
		    PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY &&
	    next.phase != PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	    !observe_remote_tone(&next_exchange, received_sample, &emitted))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (phase_transmits_tone(next.phase) &&
	    !generate_local_tone(&next_exchange, &generated))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	if (emitted.tone_event.kind ==
	    V34_PHASE2_TONE_PHASE_REVERSED)
		handle_remote_reversal(
			&next, &next_exchange,
			emitted.tone_event.reversal_delay_samples, &emitted);
	advance_reversal_wait(&next, &next_exchange, &emitted);
	if (emitted.kind !=
		    PHASE2_PROBE_TURNAROUND_EVENT_RECOVERY_STARTED &&
	    !finish_fixed_interval(&next, &next_exchange, &emitted))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	advance_signal_a_probe_start(&next, &next_exchange, &emitted);
	if (advance_signal_a_epoch)
		next.signal_a_reversal_elapsed_samples++;
	if (!phase2_probe_turnaround_is_valid(
		    &next, completed_probe, &next_exchange))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = next;
	*exchange = next_exchange;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_PROBE_TURNAROUND_OK;
}

const char *phase2_probe_turnaround_result_name(
	enum phase2_probe_turnaround_result result)
{
	switch (result) {
	case PHASE2_PROBE_TURNAROUND_OK:
		return "ok";
	case PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
