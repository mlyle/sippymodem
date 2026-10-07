// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_initial_probe.h"

#include <stddef.h>

_Static_assert(
	PHASE2_INITIAL_PROBE_REVERSAL_WAIT_BASE_SAMPLES +
		PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES <= UINT16_MAX,
	"Initial-probe reversal deadline must fit its sample counter");

static bool signal_is_b(const struct phase2_info0_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B;
}

static bool signal_b_path_is_valid(
	const struct phase2_initial_probe *probe)
{
	const struct v34_phase2_probe_acquisition *receive =
		&probe->path.receive;

	if (!v34_phase2_probe_acquisition_is_valid(receive) ||
	    probe->tone_wait_samples != 0 ||
	    probe->reversal_wait_samples_remaining >
		    PHASE2_INITIAL_PROBE_REVERSAL_WAIT_BASE_SAMPLES +
		    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES)
		return false;
	switch (probe->phase) {
	case PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING:
		return probe->tone_wait_samples == 0 &&
		       probe->failure ==
			       PHASE2_PROBE_NO_FAILURE &&
		       (receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_SEARCHING ||
			receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_RECEIVING);
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE:
		return probe->failure ==
			       PHASE2_PROBE_NO_FAILURE &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
	case PHASE2_INITIAL_PROBE_FAILED:
		switch (probe->failure) {
		case PHASE2_PROBE_REMOTE_NOT_FOUND:
			return probe->tone_wait_samples == 0 &&
			       receive->phase ==
				       V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND;
		case PHASE2_PROBE_NO_FAILURE:
		case PHASE2_PROBE_LOCAL_TRANSMIT_LIMIT:
			return false;
		}
		return false;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING:
	case PHASE2_INITIAL_PROBE_SIGNAL_A_WAIT_TONE:
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TONE:
		return false;
	}
	return false;
}

static bool signal_a_path_is_valid(
	const struct phase2_initial_probe *probe)
{
	const struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;

	if (!v34_phase2_probe_sequence_is_valid(transmit) ||
	    probe->reversal_wait_samples_remaining != 0)
		return false;
	switch (probe->phase) {
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING:
		return probe->tone_wait_samples == 0 && probe->failure ==
			       PHASE2_PROBE_NO_FAILURE &&
		       (transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L1 ||
			transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L2);
	case PHASE2_INITIAL_PROBE_SIGNAL_A_WAIT_TONE:
		return probe->tone_wait_samples <
			       PHASE2_INITIAL_PROBE_TONE_B_WAIT_SAMPLES &&
		       probe->failure == PHASE2_PROBE_NO_FAILURE &&
		       transmit->phase == V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TONE:
		return probe->tone_wait_samples == 0 && probe->failure ==
			       PHASE2_PROBE_NO_FAILURE &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE;
	case PHASE2_INITIAL_PROBE_FAILED:
		return probe->tone_wait_samples ==
			       PHASE2_INITIAL_PROBE_TONE_B_WAIT_SAMPLES &&
		       probe->failure ==
			       PHASE2_PROBE_LOCAL_TRANSMIT_LIMIT &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED;
	case PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING:
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE:
		return false;
	}
	return false;
}

bool phase2_initial_probe_is_valid(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange)
{
	if (probe == NULL ||
	    !phase2_info0_exchange_is_valid(exchange) ||
	    (unsigned int)probe->phase >
		    PHASE2_INITIAL_PROBE_FAILED ||
	    (unsigned int)probe->failure >
		    PHASE2_PROBE_LOCAL_TRANSMIT_LIMIT)
		return false;
	return signal_is_b(exchange) ?
		       signal_b_path_is_valid(probe) :
		       signal_a_path_is_valid(probe);
}

enum phase2_initial_probe_result
phase2_initial_probe_init(
	struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	const struct phase2_ranging_control *completed_ranging)
{
	struct phase2_initial_probe initialized = { 0 };

	if (probe == NULL ||
	    !phase2_ranging_control_is_valid(completed_ranging, exchange, true) ||
	    completed_ranging->phase != PHASE2_RANGING_PROBE_READY ||
	    exchange->transmit_phase != PHASE2_INFO0_TONE)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (signal_is_b(exchange)) {
		uint32_t deadline = PHASE2_INITIAL_PROBE_REVERSAL_WAIT_BASE_SAMPLES +
			completed_ranging->round_trip_delay_samples;
		uint32_t elapsed = completed_ranging->probe_reversal_elapsed_samples;

		initialized.phase =
			PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING;
		initialized.reversal_wait_samples_remaining =
			deadline > elapsed ? deadline - elapsed : 0;
		if (v34_phase2_probe_acquisition_init(
			    &initialized.path.receive) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	} else {
		initialized.phase =
			PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING;
		if (v34_phase2_probe_sequence_init(
			    &initialized.path.transmit,
			    v34_phase2_tone_nominal_amplitude(&exchange->tone_transmitter),
			    completed_ranging->round_trip_delay_samples) !=
		    V34_PHASE2_PROBE_SEQUENCE_OK)
			return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	}
	if (!phase2_initial_probe_is_valid(&initialized, exchange))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return PHASE2_INITIAL_PROBE_OK;
}

static bool reset_remote_tone_detector(
	struct phase2_info0_exchange *exchange)
{
	return v34_phase2_tone_detector_reset(
		       &exchange->tone_detector) ==
	       V34_PHASE2_TONE_OK;
}

static void fail_probe(
	struct phase2_initial_probe *probe,
	enum phase2_probe_failure failure,
	struct phase2_initial_probe_event *event)
{
	probe->phase = PHASE2_INITIAL_PROBE_FAILED;
	probe->failure = failure;
	event->kind = PHASE2_INITIAL_PROBE_EVENT_FAILED;
	event->failure = failure;
}

static bool receive_remote_probe(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct v34_phase2_probe_acquisition *receive =
		&probe->path.receive;

	*transmit_sample = 0;
	if (receive->receiver.phase ==
	    V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END) {
		if (v34_phase2_probe_acquisition_sample(
			    receive, received_sample, true,
			    &event->receive_event) !=
			    V34_PHASE2_PROBE_ACQUISITION_OK ||
		    !reset_remote_tone_detector(exchange) ||
		    !phase2_info0_exchange_observe_tone(
			    exchange, received_sample, &event->tone_event) ||
		    !phase2_info0_exchange_generate_tone(
			    exchange, transmit_sample))
			return false;
		probe->phase =
			PHASE2_INITIAL_PROBE_SIGNAL_B_TONE;
		event->kind =
			PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE;
		return true;
	}
	if (v34_phase2_probe_acquisition_sample(
		    receive, received_sample, false, &event->receive_event) !=
	    V34_PHASE2_PROBE_ACQUISITION_OK)
		return false;
	switch (event->receive_event.kind) {
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND:
		fail_probe(
			probe,
			PHASE2_PROBE_REMOTE_NOT_FOUND,
			event);
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY:
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED:
		return false;
	}
	return true;
}

static bool transmit_local_probe(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;
	bool remote_tone_started;

	if (transmit->phase == V34_PHASE2_PROBE_SEQUENCE_PHASE_L2 ||
	    transmit->phase == V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED) {
		if (transmit->samples_emitted ==
			    V34_PHASE2_PROBE_L1_SAMPLES &&
		    !reset_remote_tone_detector(exchange))
			return false;
		if (!phase2_info0_exchange_observe_tone(
			    exchange, received_sample, &event->tone_event))
			return false;
	}
	remote_tone_started = event->tone_event.kind ==
			      V34_PHASE2_TONE_STARTED;
	if (remote_tone_started &&
	    transmit->samples_emitted > V34_PHASE2_PROBE_L1_SAMPLES) {
		if (v34_phase2_probe_sequence_sample(
			    transmit, true, transmit_sample,
			    &event->transmit_event) !=
		    V34_PHASE2_PROBE_SEQUENCE_OK)
			return false;
		probe->phase =
			PHASE2_INITIAL_PROBE_SIGNAL_A_TONE;
		probe->tone_wait_samples = 0;
		event->kind =
			PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE;
		return true;
	}
	if (v34_phase2_probe_sequence_sample(
		    transmit, false, transmit_sample, &event->transmit_event) !=
	    V34_PHASE2_PROBE_SEQUENCE_OK)
		return false;
	if (event->transmit_event.kind ==
	    V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED)
		probe->phase = PHASE2_INITIAL_PROBE_SIGNAL_A_WAIT_TONE;
	return true;
}

static bool wait_for_remote_tone(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	if (!transmit_local_probe(probe, exchange, received_sample,
				  transmit_sample, event))
		return false;
	if (probe->phase == PHASE2_INITIAL_PROBE_SIGNAL_A_WAIT_TONE &&
	    ++probe->tone_wait_samples == PHASE2_INITIAL_PROBE_TONE_B_WAIT_SAMPLES)
		fail_probe(
			probe,
			PHASE2_PROBE_LOCAL_TRANSMIT_LIMIT,
			event);
	return true;
}

enum phase2_initial_probe_result
phase2_initial_probe_sample(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct phase2_initial_probe next;
	struct phase2_info0_exchange next_exchange;
	struct phase2_initial_probe_event emitted = { 0 };
	int16_t generated = 0;
	bool accepted = true;

	if (!phase2_initial_probe_is_valid(probe, exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	next_exchange = *exchange;
	if (next.reversal_wait_samples_remaining != 0 &&
	    next.phase != PHASE2_INITIAL_PROBE_FAILED)
		next.reversal_wait_samples_remaining--;
	switch (next.phase) {
	case PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING:
		accepted = receive_remote_probe(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING:
		accepted = transmit_local_probe(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_WAIT_TONE:
		accepted = wait_for_remote_tone(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TONE:
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE:
		accepted = phase2_info0_exchange_observe_tone(
				   &next_exchange, received_sample,
				   &emitted.tone_event) &&
			   phase2_info0_exchange_generate_tone(
				   &next_exchange, &generated);
		break;
	case PHASE2_INITIAL_PROBE_FAILED:
		break;
	}
	if (!accepted ||
	    !phase2_initial_probe_is_valid(&next, &next_exchange))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*exchange = next_exchange;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result
phase2_initial_probe_measure(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!phase2_initial_probe_is_valid(probe, exchange) ||
	    !signal_is_b(exchange) ||
	    measurement == NULL ||
	    v34_phase2_probe_acquisition_measure(
		    &probe->path.receive, measurement) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_INITIAL_PROBE_OK;
}
